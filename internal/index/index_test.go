package index

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

func sample() *Index {
	ix := New([]string{"/data"})
	root := ix.Add(Entry{Name: "/data", Parent: NoParent, IsDir: true})
	docs := ix.Add(Entry{Name: "Docs", Parent: root, IsDir: true})
	ix.Add(Entry{Name: "Report.PDF", Parent: docs, Size: 10})
	src := ix.Add(Entry{Name: "src", Parent: root, IsDir: true})
	ix.Add(Entry{Name: "main.go", Parent: src, Size: 20})
	return ix
}

func TestPathAndLowercaseAndExt(t *testing.T) {
	ix := sample()
	if got := ix.Path(2); got != filepath.Join("/data", "Docs", "Report.PDF") {
		t.Errorf("path = %q", got)
	}
	if got := ix.Path(0); got != "/data" {
		t.Errorf("root path = %q", got)
	}
	if ix.Lower[2] != "report.pdf" || ix.Ext(2) != "pdf" || ix.Ext(1) != "" {
		t.Errorf("lower/ext wrong: %q %q %q", ix.Lower[2], ix.Ext(2), ix.Ext(1))
	}
}

func TestRootWithTrailingSeparatorDoesNotDouble(t *testing.T) {
	ix := New([]string{"/"})
	root := ix.Add(Entry{Name: "/", Parent: NoParent, IsDir: true})
	etc := ix.Add(Entry{Name: "etc", Parent: root, IsDir: true})
	if got := ix.Path(etc); got != "/etc" {
		t.Errorf("path = %q", got)
	}
}

func TestCompactDropsSubtreesAndRenumbers(t *testing.T) {
	ix := sample()
	ix.Remove(1) // Docs; Report.PDF below it becomes an orphan
	if ix.Len() != 4 {
		t.Fatalf("live count = %d", ix.Len())
	}
	ix.Compact()
	if len(ix.Entries) != 3 {
		t.Fatalf("after compact %d entries", len(ix.Entries))
	}
	if got := ix.Path(2); got != filepath.Join("/data", "src", "main.go") {
		t.Errorf("renumbered path = %q", got)
	}
}

func TestSaveLoadRoundTrip(t *testing.T) {
	ix := sample()
	ix.Entries[2].Modified = 1700000000
	ix.Entries[2].Created = 1600000000
	ix.BuiltAt = time.Unix(1234567890, 0)
	path := filepath.Join(t.TempDir(), "idx", "index.bin")
	if err := ix.Save(path); err != nil {
		t.Fatal(err)
	}
	loaded, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(loaded.Entries) != 5 || loaded.Entries[2] != ix.Entries[2] || loaded.Roots[0] != "/data" {
		t.Errorf("roundtrip mismatch: %+v", loaded.Entries)
	}
	if !loaded.BuiltAt.Equal(ix.BuiltAt) || loaded.Lower[2] != "report.pdf" {
		t.Errorf("metadata mismatch")
	}
	if _, err := Load(filepath.Join(t.TempDir(), "missing")); err != ErrNotFound {
		t.Errorf("missing file: %v", err)
	}
}

func TestAddTreeScansAndExcludes(t *testing.T) {
	dir := t.TempDir()
	mustWrite(t, filepath.Join(dir, "a", "one.txt"), 5)
	mustWrite(t, filepath.Join(dir, "a", "deep", "two.txt"), 7)
	mustWrite(t, filepath.Join(dir, "node_modules", "pkg", "x.js"), 1)
	mustWrite(t, filepath.Join(dir, "skip.tmp"), 1)
	ex, err := NewExcludes([]string{"node_modules", "*.tmp"})
	if err != nil {
		t.Fatal(err)
	}
	ix := New([]string{dir})
	res, err := ix.AddTree(dir, NoParent, ex, nil)
	if err != nil {
		t.Fatal(err)
	}
	if res.Start != 0 || int(res.End) != len(ix.Entries) {
		t.Errorf("range %v", res)
	}
	paths := map[string]Entry{}
	for i := range ix.Entries {
		paths[ix.Path(uint32(i))] = ix.Entries[i]
	}
	if len(paths) != 5 {
		t.Errorf("got %d entries: %v", len(paths), paths)
	}
	if e := paths[filepath.Join(dir, "a", "deep", "two.txt")]; e.Size != 7 || e.IsDir {
		t.Errorf("two.txt = %+v", e)
	}
	if _, ok := paths[filepath.Join(dir, "node_modules")]; ok {
		t.Error("node_modules should be excluded")
	}
}

func TestExcludePathPatternsCoverSubtrees(t *testing.T) {
	ex, err := NewExcludes([]string{"/proc", "**/.git", "~/Library/Caches"})
	if err != nil {
		t.Fatal(err)
	}
	home, _ := os.UserHomeDir()
	for _, p := range []string{"/proc", "/proc/1/status", "/x/y/.git/HEAD", home + "/Library/Caches/foo"} {
		if !ex.Match(p, filepath.Base(p)) {
			t.Errorf("%s should match", p)
		}
	}
	if ex.Match("/process", "process") {
		t.Error("/process should not match")
	}
	if _, err := NewExcludes([]string{"[bad"}); err == nil {
		t.Error("expected invalid pattern error")
	}
}

func mustWrite(t *testing.T, path string, size int) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, make([]byte, size), 0o644); err != nil {
		t.Fatal(err)
	}
}
