package search

import (
	"cmp"
	"fmt"
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
	}
	return 0, fmt.Errorf("unknown sort key %q (use path, name, size, dm, dc or ext)", s)
}

func Sort(ix *index.Index, hits []uint32, key SortKey, descending bool) {
	byName := func(a, b uint32) int {
		return cmp.Or(strings.Compare(ix.Lower[a], ix.Lower[b]), strings.Compare(ix.Entries[a].Name, ix.Entries[b].Name), cmp.Compare(a, b))
	}
	var less func(a, b uint32) int
	switch key {
	case SortPath:
		paths := make([]string, len(ix.Entries))
		for _, h := range hits {
			paths[h] = strings.ToLower(ix.Path(h))
		}
		less = func(a, b uint32) int { return cmp.Or(strings.Compare(paths[a], paths[b]), byName(a, b)) }
	case SortName:
		less = byName
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
	}
	if descending {
		inner := less
		less = func(a, b uint32) int { return inner(b, a) }
	}
	slices.SortFunc(hits, less)
}
