package search

import (
	"path/filepath"
	"reflect"
	"sort"
	"testing"
	"time"

	"eind/internal/index"
	"eind/internal/query"
)

var (
	now       = time.Date(2024, 3, 13, 12, 0, 0, 0, time.UTC)
	yesterday = now.Add(-24 * time.Hour).Unix()
	lastYear  = now.AddDate(-1, 0, 0).Unix()
	root      = filepath.Join(string(filepath.Separator), "data")
)

func sample() *index.Index {
	ix := index.New([]string{root})
	r := ix.Add(index.Entry{Name: root, Parent: index.NoParent, IsDir: true, Modified: lastYear})
	docs := ix.Add(index.Entry{Name: "Docs", Parent: r, IsDir: true, Modified: lastYear})
	ix.Add(index.Entry{Name: "Report Final.PDF", Parent: docs, Size: 5 << 20, Modified: yesterday})
	ix.Add(index.Entry{Name: "report-draft.pdf", Parent: docs, Size: 100, Modified: lastYear})
	ix.Add(index.Entry{Name: "notes.md", Parent: docs, Size: 10, Modified: lastYear})
	src := ix.Add(index.Entry{Name: "src", Parent: r, IsDir: true, Modified: lastYear})
	ix.Add(index.Entry{Name: "main.go", Parent: src, Size: 2000, Modified: yesterday})
	ix.Add(index.Entry{Name: "main_test.go", Parent: src, Size: 300, Modified: lastYear})
	deep := ix.Add(index.Entry{Name: "pkg", Parent: src, IsDir: true, Modified: lastYear})
	ix.Add(index.Entry{Name: "log", Parent: deep, Size: 1, Modified: lastYear})
	ix.Add(index.Entry{Name: "catalog.txt", Parent: deep, Size: 1, Modified: lastYear})
	return ix
}

func names(ix *index.Index, hits []uint32) []string {
	var out []string
	for _, h := range hits {
		out = append(out, ix.Entries[h].Name)
	}
	sort.Strings(out)
	return out
}

func TestQuerySemantics(t *testing.T) {
	ix := sample()
	cases := []struct {
		query    string
		defaults query.Defaults
		want     []string
	}{
		{"report", query.Defaults{}, []string{"Report Final.PDF", "report-draft.pdf"}},
		{"case:Report", query.Defaults{}, []string{"Report Final.PDF"}},
		{"report !draft", query.Defaults{}, []string{"Report Final.PDF"}},
		{"notes|catalog", query.Defaults{}, []string{"catalog.txt", "notes.md"}},
		{"*.go", query.Defaults{}, []string{"main.go", "main_test.go"}},
		{"*.PDF", query.Defaults{}, []string{"Report Final.PDF", "report-draft.pdf"}},
		{"case:*.PDF", query.Defaults{}, []string{"Report Final.PDF"}},
		{"main*", query.Defaults{}, []string{"main.go", "main_test.go"}},
		{"*ain*", query.Defaults{}, []string{"main.go", "main_test.go"}},
		{"* ext:md", query.Defaults{}, []string{"notes.md"}},
		{"*a*.go", query.Defaults{}, []string{"main.go", "main_test.go"}},
		{"main?go", query.Defaults{}, []string{"main.go"}},
		{"ww:log", query.Defaults{}, []string{"log"}},
		{"ww:main", query.Defaults{}, []string{"main.go", "main_test.go"}},
		{"wfn:notes.md", query.Defaults{}, []string{"notes.md"}},
		{`regex:^main.*\.go$`, query.Defaults{}, []string{"main.go", "main_test.go"}},
		{"ext:pdf;md", query.Defaults{}, []string{"Report Final.PDF", "notes.md", "report-draft.pdf"}},
		{"size:>1mb", query.Defaults{}, []string{"Report Final.PDF"}},
		{"size:100..2000 file:", query.Defaults{}, []string{"main.go", "main_test.go", "report-draft.pdf"}},
		{"dm:yesterday", query.Defaults{}, []string{"Report Final.PDF", "main.go"}},
		{"dm:>2024-03-11 ext:go", query.Defaults{}, []string{"main.go"}},
		{"folder:", query.Defaults{}, []string{root, "Docs", "pkg", "src"}},
		{"folder:pkg", query.Defaults{}, []string{"pkg"}},
		{"path:src file:", query.Defaults{}, []string{"catalog.txt", "log", "main.go", "main_test.go"}},
		{"src", query.Defaults{MatchPath: true, WholeWord: true}, []string{"catalog.txt", "log", "main.go", "main_test.go", "pkg", "src"}},
		{"len:>15", query.Defaults{}, []string{"Report Final.PDF", "report-draft.pdf"}},
		{"depth:4", query.Defaults{}, []string{"catalog.txt", "log"}},
		{"parent:" + filepath.Join(root, "SRC"), query.Defaults{}, []string{"main.go", "main_test.go", "pkg"}},
		{"infolder:" + filepath.Join(root, "src") + " ext:txt", query.Defaults{}, []string{"catalog.txt"}},
		{"parent:" + filepath.Join(root, "nope"), query.Defaults{}, nil},
		{`"report final"`, query.Defaults{}, []string{"Report Final.PDF"}},
		{"<notes|log> !ext:md", query.Defaults{}, []string{"catalog.txt", "log"}},
	}
	for _, c := range cases {
		c.defaults.Now = now
		node, err := query.Parse(c.query, c.defaults)
		if err != nil {
			t.Fatalf("%q: parse: %v", c.query, err)
		}
		hits, err := Run(ix, node)
		if err != nil {
			t.Fatalf("%q: run: %v", c.query, err)
		}
		if got := names(ix, hits); !reflect.DeepEqual(got, c.want) {
			t.Errorf("%q: got %v, want %v", c.query, got, c.want)
		}
	}
}

func TestInvalidRegexIsReported(t *testing.T) {
	node, _ := query.Parse("regex:(", query.Defaults{})
	if _, err := Run(sample(), node); err == nil {
		t.Error("expected error")
	}
}

func TestTombstonedEntriesAreSkipped(t *testing.T) {
	ix := sample()
	ix.Remove(4) // notes.md
	hits, _ := Run(ix, query.MatchAll)
	if len(hits) != len(ix.Entries)-1 {
		t.Errorf("got %d hits", len(hits))
	}
}

func TestSortOrders(t *testing.T) {
	ix := sample()
	hits, _ := Run(ix, query.Ext{Exts: []string{"pdf", "go", "md"}})
	first := func(key SortKey, desc bool) string {
		Sort(ix, hits, key, desc)
		return ix.Entries[hits[0]].Name
	}
	cases := []struct {
		key  SortKey
		desc bool
		want string
	}{
		{SortName, false, "main.go"},
		{SortName, true, "report-draft.pdf"},
		{SortSize, true, "Report Final.PDF"},
		{SortSize, false, "notes.md"},
		{SortModified, true, "Report Final.PDF"},
		{SortExt, false, "main.go"},
		{SortPath, false, "notes.md"},
	}
	for _, c := range cases {
		if got := first(c.key, c.desc); got != c.want {
			t.Errorf("sort %v desc=%v: first = %q, want %q", c.key, c.desc, got, c.want)
		}
	}
}
