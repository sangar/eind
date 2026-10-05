package index

import (
	"os"
	"path/filepath"
	"slices"
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

func TestPathAndExt(t *testing.T) {
	ix := sample()
	if got := ix.Path(2); got != filepath.Join("/data", "Docs", "Report.PDF") {
		t.Errorf("path = %q", got)
	}
	if got := ix.Path(0); got != "/data" {
		t.Errorf("root path = %q", got)
	}
	if Ext(ix.Name(2)) != "pdf" || Ext(ix.Name(1)) != "" {
		t.Errorf("ext wrong: %q %q", Ext(ix.Name(2)), Ext(ix.Name(1)))
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

func paths(ix *Index) []string {
	var out []string
	for i := range uint32(ix.Count()) {
		if ix.Live(i) {
			out = append(out, ix.Path(i))
		}
	}
	return out
}

func TestSaveDropsRemovedSubtreesAndStoresPathOrder(t *testing.T) {
	ix := sample()
	ix.Add(Entry{Name: "src.txt", Parent: 0})
	ix.Add(Entry{Name: "SRC-old", Parent: 0, IsDir: true})
	ix.Remove(1) // Docs; Report.PDF below it becomes an orphan
	path := filepath.Join(t.TempDir(), "idx", "index.bin")
	if err := ix.Save(path); err != nil {
		t.Fatal(err)
	}
	// Comparing lowercased paths puts "SRC-old" and "src.txt" between "src"
	// and "src/main.go", because '-' and '.' sort before the separator.
	want := []string{"/data", "/data/src", "/data/SRC-old", "/data/src.txt", "/data/src/main.go"}
	if got := paths(ix); !slices.Equal(got, want) {
		t.Errorf("after save %q, want %q", got, want)
	}
}

func TestSaveLoadRoundTrip(t *testing.T) {
	ix := sample()
	ix.Update(2, 10, 1700000000, 1600000000)
	ix.BuiltAt = time.Unix(1234567890, 0)
	path := filepath.Join(t.TempDir(), "idx", "index.bin")
	if err := ix.Save(path); err != nil {
		t.Fatal(err)
	}
	loaded, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	want := Entry{Name: "Report.PDF", Parent: 1, Size: 10, Modified: 1700000000, Created: 1600000000}
	if loaded.Count() != 5 || loaded.Entry(2) != want || loaded.Roots[0] != "/data" {
		t.Errorf("roundtrip mismatch: %+v", loaded.Entry(2))
	}
	if !loaded.BuiltAt.Equal(ix.BuiltAt) {
		t.Errorf("built at %v", loaded.BuiltAt)
	}
	if _, err := Load(filepath.Join(t.TempDir(), "missing")); err != ErrNotFound {
		t.Errorf("missing file: %v", err)
	}
}

func savedSample(t *testing.T) (*Index, string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "index.bin")
	ix := sample()
	if err := ix.Save(path); err != nil {
		t.Fatal(err)
	}
	if err := ix.EnableJournal(path); err != nil {
		t.Fatal(err)
	}
	return ix, path
}

func TestJournalShowsChangesToOtherLoads(t *testing.T) {
	ix, path := savedSample(t)
	ix.Add(Entry{Name: "new.go", Parent: 3, Size: 3})
	ix.Remove(1)
	ix.Remove(2)
	ix.Update(4, 99, 5, 6)
	if err := ix.Flush(); err != nil {
		t.Fatal(err)
	}
	loaded, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	want := []string{"/data", "/data/src", "/data/src/main.go", "/data/src/new.go"}
	if got := paths(loaded); !slices.Equal(got, want) {
		t.Errorf("paths %q, want %q", got, want)
	}
	if loaded.Size(4) != 99 || loaded.Modified(4) != 5 || loaded.Created(4) != 6 {
		t.Errorf("update lost: %+v", loaded.Entry(4))
	}
}

func TestJournalIgnoresHalfWrittenEntry(t *testing.T) {
	ix, path := savedSample(t)
	ix.Add(Entry{Name: "kept.go", Parent: 3})
	if err := ix.Flush(); err != nil {
		t.Fatal(err)
	}
	f, err := os.OpenFile(journalPath(path), os.O_WRONLY|os.O_APPEND, 0)
	if err != nil {
		t.Fatal(err)
	}
	f.Write([]byte{opAdd, 3, 0})
	f.Close()
	loaded, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if loaded.Count() != 6 || loaded.Name(5) != "kept.go" {
		t.Errorf("count %d", loaded.Count())
	}
	// A writer continuing the journal first cuts the torn entry off.
	if err := loaded.EnableJournal(path); err != nil {
		t.Fatal(err)
	}
	loaded.Add(Entry{Name: "after.go", Parent: 3})
	if err := loaded.Flush(); err != nil {
		t.Fatal(err)
	}
	again, err := Load(path)
	if err != nil || again.Count() != 7 || again.Name(6) != "after.go" {
		t.Errorf("after torn entry: %v %d", err, again.Count())
	}
}

func TestFlushReportsIndexWrittenByAnotherProcess(t *testing.T) {
	ix, path := savedSample(t)
	other, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := other.Save(path); err != nil {
		t.Fatal(err)
	}
	ix.Add(Entry{Name: "lost.go", Parent: 3})
	if err := ix.Flush(); err != ErrReplaced {
		t.Errorf("flush = %v, want ErrReplaced", err)
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
	if res.Start != 0 || int(res.End) != ix.Count() {
		t.Errorf("range %v", res)
	}
	paths := map[string]Entry{}
	for i := range uint32(ix.Count()) {
		paths[ix.Path(i)] = ix.Entry(i)
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
