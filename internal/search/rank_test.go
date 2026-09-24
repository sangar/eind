package search

import (
	"reflect"
	"slices"
	"testing"

	"eind/internal/index"
	"eind/internal/query"
)

func TestRankPrefersExactThenPrefixThenWordThenDepth(t *testing.T) {
	ix := index.New([]string{"/r"})
	r := ix.Add(index.Entry{Name: "/r", Parent: index.NoParent, IsDir: true})
	deep := ix.Add(index.Entry{Name: "deep", Parent: r, IsDir: true})
	ix.Add(index.Entry{Name: "my-readme-copy.txt", Parent: r})
	ix.Add(index.Entry{Name: "README.md", Parent: deep})
	ix.Add(index.Entry{Name: "readme-first.txt", Parent: r})
	ix.Add(index.Entry{Name: "unreadmeable", Parent: r})
	ix.Add(index.Entry{Name: "readme", Parent: r, IsDir: true})

	node, _ := query.Parse("readme", query.Defaults{})
	hits, _ := Run(ix, node)
	names := func(hits []uint32) []string {
		var got []string
		for _, h := range hits {
			got = append(got, ix.Entries[h].Name)
		}
		return got
	}
	want := []string{"readme", "README.md", "readme-first.txt", "my-readme-copy.txt", "unreadmeable"}
	if got := names(Rank(ix, slices.Clone(hits), node, -1)); !reflect.DeepEqual(got, want) {
		t.Errorf("full: got %v, want %v", got, want)
	}
	// keep == 1 goes through the heap selection rather than the sort
	if got := names(Rank(ix, slices.Clone(hits), node, 1)); !reflect.DeepEqual(got, want[:1]) {
		t.Errorf("top-1: got %v, want %v", got, want[:1])
	}
	if got := Rank(ix, slices.Clone(hits), node, 0); len(got) != 0 {
		t.Errorf("top-0: got %v", got)
	}
}

func TestRankWithoutNameTermsFallsBackToNameOrder(t *testing.T) {
	ix := sample()
	node, _ := query.Parse("ext:go", query.Defaults{})
	hits, _ := Run(ix, node)
	hits = Rank(ix, hits, node, -1)
	if ix.Entries[hits[0]].Name != "main.go" {
		t.Errorf("first = %q", ix.Entries[hits[0]].Name)
	}
}
