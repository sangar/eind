package search

import (
	"cmp"
	"fmt"
	"os"
	"slices"
	"strings"

	"eind/internal/index"
)

type SortKey int

const (
	SortPath SortKey = iota
	SortName
	SortSize
	SortModified
	SortCreated
	SortExt
	// SortRelevance is handled by Rank, which needs the query; Sort treats it as SortName.
	SortRelevance
)

func ParseSortKey(s string) (SortKey, error) {
	switch strings.ToLower(s) {
	case "path":
		return SortPath, nil
	case "name":
		return SortName, nil
	case "size":
		return SortSize, nil
	case "dm", "modified", "date-modified":
		return SortModified, nil
	case "dc", "created", "date-created":
		return SortCreated, nil
	case "ext", "extension":
		return SortExt, nil
	case "relevance", "rank":
		return SortRelevance, nil
	}
	return 0, fmt.Errorf("unknown sort key %q (use path, name, size, dm, dc, ext or relevance)", s)
}

// Sort orders all hits by key.
func Sort(ix *index.Index, hits []uint32, key SortKey, descending bool) {
	if key == SortPath {
		sortByPath(ix, hits, descending)
		return
	}
	slices.SortFunc(hits, comparator(ix, key, descending))
}

// Top returns the first keep hits in sort order. Selecting a few hundred out
// of a million is far cheaper than sorting them all, and for paths it avoids
// building a path string per hit; keep < 0 sorts and returns everything.
func Top(ix *index.Index, hits []uint32, key SortKey, descending bool, keep int) []uint32 {
	if keep < 0 || keep >= len(hits) {
		Sort(ix, hits, key, descending)
		return hits
	}
	if keep*4 > len(hits) {
		Sort(ix, hits, key, descending)
		return hits[:keep]
	}
	return topK(hits, keep, comparator(ix, key, descending))
}

// sortByPath builds each hit's lowercased path once, which beats comparing
// parent chains when every hit has to be placed.
func sortByPath(ix *index.Index, hits []uint32, descending bool) {
	type keyed struct {
		path string
		hit  uint32
	}
	rows := make([]keyed, len(hits))
	for i, h := range hits {
		rows[i] = keyed{strings.ToLower(ix.Path(h)), h}
	}
	byName := nameComparator(ix)
	less := func(a, b keyed) int { return cmp.Or(strings.Compare(a.path, b.path), byName(a.hit, b.hit)) }
	if descending {
		slices.SortFunc(rows, func(a, b keyed) int { return less(b, a) })
	} else {
		slices.SortFunc(rows, less)
	}
	for i := range rows {
		hits[i] = rows[i].hit
	}
}

func nameComparator(ix *index.Index) func(a, b uint32) int {
	return func(a, b uint32) int {
		return cmp.Or(strings.Compare(ix.Lower[a], ix.Lower[b]), strings.Compare(ix.Entries[a].Name, ix.Entries[b].Name), cmp.Compare(a, b))
	}
}

func comparator(ix *index.Index, key SortKey, descending bool) func(a, b uint32) int {
	byName := nameComparator(ix)
	var less func(a, b uint32) int
	switch key {
	case SortPath:
		less = func(a, b uint32) int { return cmp.Or(comparePaths(ix, a, b), byName(a, b)) }
	case SortSize:
		less = func(a, b uint32) int {
			return cmp.Or(cmp.Compare(ix.Entries[a].Size, ix.Entries[b].Size), byName(a, b))
		}
	case SortModified:
		less = func(a, b uint32) int {
			return cmp.Or(cmp.Compare(ix.Entries[a].Modified, ix.Entries[b].Modified), byName(a, b))
		}
	case SortCreated:
		less = func(a, b uint32) int {
			return cmp.Or(cmp.Compare(ix.Entries[a].Created, ix.Entries[b].Created), byName(a, b))
		}
	case SortExt:
		less = func(a, b uint32) int { return cmp.Or(strings.Compare(ix.Ext(a), ix.Ext(b)), byName(a, b)) }
	default:
		less = byName
	}
	if descending {
		return func(a, b uint32) int { return less(b, a) }
	}
	return less
}

// comparePaths orders two entries exactly as strings.Compare would order
// their lowercased full paths, but walks the parent chains byte by byte
// instead of building the paths.
func comparePaths(ix *index.Index, a, b uint32) int {
	var ca, cb [64]uint32
	ra, okA := chain(ix, a, &ca)
	rb, okB := chain(ix, b, &cb)
	if !okA || !okB {
		return strings.Compare(strings.ToLower(ix.Path(a)), strings.ToLower(ix.Path(b)))
	}
	x := pathCursor{ix: ix, comps: ra}
	y := pathCursor{ix: ix, comps: rb}
	for {
		p, moreX := x.next()
		q, moreY := y.next()
		switch {
		case !moreX && !moreY:
			return 0
		case !moreX:
			return -1
		case !moreY:
			return 1
		case p != q:
			return cmp.Compare(p, q)
		}
	}
}

// chain fills buf with the entries from the root down to i.
func chain(ix *index.Index, i uint32, buf *[64]uint32) ([]uint32, bool) {
	n := 0
	for {
		if n == len(buf) {
			return nil, false
		}
		buf[n] = i
		n++
		p := ix.Entries[i].Parent
		if p == index.NoParent {
			break
		}
		i = p
	}
	slices.Reverse(buf[:n])
	return buf[:n], true
}

// pathCursor yields the bytes of a lowercased path the way Path joins it: a
// separator between components unless the previous one already ends with it.
type pathCursor struct {
	ix     *index.Index
	comps  []uint32
	ci     int
	off    int
	joined bool
}

func (c *pathCursor) next() (byte, bool) {
	for c.ci < len(c.comps) {
		name := c.ix.Lower[c.comps[c.ci]]
		if c.ci > 0 && !c.joined {
			c.joined = true
			if !strings.HasSuffix(c.ix.Lower[c.comps[c.ci-1]], string(os.PathSeparator)) {
				return os.PathSeparator, true
			}
		}
		if c.off < len(name) {
			b := name[c.off]
			c.off++
			return b, true
		}
		c.ci++
		c.off = 0
		c.joined = false
	}
	return 0, false
}
