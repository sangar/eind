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
// of a million is far cheaper than sorting them all, except by path, which
// costs almost nothing; keep < 0 sorts and returns everything.
func Top(ix *index.Index, hits []uint32, key SortKey, descending bool, keep int) []uint32 {
	if keep < 0 || keep >= len(hits) {
		Sort(ix, hits, key, descending)
		return hits
	}
	if keep*4 > len(hits) || key == SortPath {
		Sort(ix, hits, key, descending)
		return hits[:keep]
	}
	return topK(hits, keep, comparator(ix, key, descending))
}

// sortByPath relies on the base storing entries in path order, so base hits
// sort by id; only entries added since need their paths compared, and they
// are merged in.
func sortByPath(ix *index.Index, hits []uint32, descending bool) {
	slices.Sort(hits)
	split, _ := slices.BinarySearch(hits, uint32(ix.BaseCount()))
	base, added := hits[:split], slices.Clone(hits[split:])
	if len(added) > 0 {
		less := func(a, b uint32) int { return cmp.Or(comparePaths(ix, a, b), compareNames(ix, a, b)) }
		slices.SortFunc(added, less)
		merged := make([]uint32, 0, len(hits))
		for _, a := range added {
			at, _ := slices.BinarySearchFunc(base, a, less)
			merged = append(append(merged, base[:at]...), a)
			base = base[at:]
		}
		copy(hits, append(merged, base...))
	}
	if descending {
		slices.Reverse(hits)
	}
}

func nameComparator(ix *index.Index) func(a, b uint32) int {
	return func(a, b uint32) int { return compareNames(ix, a, b) }
}

// compareNames orders by lowercased name, then name, then id. Base names are
// numbered in that order, so comparing two base entries needs no strings.
func compareNames(ix *index.Index, a, b uint32) int {
	na, okA := ix.NameID(a)
	nb, okB := ix.NameID(b)
	if okA && okB {
		return cmp.Or(cmp.Compare(na, nb), cmp.Compare(a, b))
	}
	x, y := ix.Name(a), ix.Name(b)
	return cmp.Or(strings.Compare(index.Lower(x), index.Lower(y)), strings.Compare(x, y), cmp.Compare(a, b))
}

func comparator(ix *index.Index, key SortKey, descending bool) func(a, b uint32) int {
	byName := nameComparator(ix)
	var less func(a, b uint32) int
	switch key {
	case SortSize:
		less = func(a, b uint32) int {
			return cmp.Or(cmp.Compare(ix.Size(a), ix.Size(b)), byName(a, b))
		}
	case SortModified:
		less = func(a, b uint32) int {
			return cmp.Or(cmp.Compare(ix.Modified(a), ix.Modified(b)), byName(a, b))
		}
	case SortCreated:
		less = func(a, b uint32) int {
			return cmp.Or(cmp.Compare(ix.Created(a), ix.Created(b)), byName(a, b))
		}
	case SortExt:
		less = func(a, b uint32) int {
			return cmp.Or(strings.Compare(index.Ext(ix.Name(a)), index.Ext(ix.Name(b))), byName(a, b))
		}
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
		p := ix.Parent(i)
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
		name := index.Lower(c.ix.Name(c.comps[c.ci]))
		if c.ci > 0 && !c.joined {
			c.joined = true
			if !strings.HasSuffix(c.ix.Name(c.comps[c.ci-1]), string(os.PathSeparator)) {
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
