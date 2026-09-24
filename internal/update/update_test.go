package update

import (
	"os"
	"path/filepath"
	"testing"

	"eind/internal/index"
)

func setup(t *testing.T) (dir string, ix *index.Index, u *Updater) {
	t.Helper()
	dir = t.TempDir()
	write(t, filepath.Join(dir, "docs", "a.txt"), 1)
	write(t, filepath.Join(dir, "docs", "sub", "b.txt"), 1)
	write(t, filepath.Join(dir, "top.txt"), 1)
	ex, _ := index.NewExcludes([]string{"*.tmp"})
	ix = index.New([]string{dir})
	if _, err := ix.AddTree(dir, index.NoParent, ex, nil); err != nil {
		t.Fatal(err)
	}
	return dir, ix, New(ix, ex)
}

func livePaths(ix *index.Index) map[string]index.Entry {
	out := map[string]index.Entry{}
	for i := range ix.Entries {
		if ix.Live(uint32(i)) {
			out[ix.Path(uint32(i))] = ix.Entries[i]
		}
	}
	return out
}

func TestReconcileAddsModifiesAndRemoves(t *testing.T) {
	dir, ix, u := setup(t)

	added := filepath.Join(dir, "docs", "new.txt")
	write(t, added, 3)
	if !u.Reconcile(added) {
		t.Fatal("adding a file should change the index")
	}
	if e, ok := livePaths(ix)[added]; !ok || e.Size != 3 {
		t.Fatalf("new file not indexed: %+v", e)
	}

	write(t, added, 9)
	if !u.Reconcile(added) || livePaths(ix)[added].Size != 9 {
		t.Fatal("size change not applied")
	}
	if u.Reconcile(added) {
		t.Error("unchanged file should not dirty the index")
	}

	os.Remove(added)
	if !u.Reconcile(added) {
		t.Fatal("removal should change the index")
	}
	if _, ok := livePaths(ix)[added]; ok {
		t.Error("removed file still indexed")
	}
}

func TestReconcileScansNewDirectoriesAndDropsRemovedOnes(t *testing.T) {
	dir, ix, u := setup(t)

	moved := filepath.Join(dir, "archive")
	if err := os.Rename(filepath.Join(dir, "docs"), moved); err != nil {
		t.Fatal(err)
	}
	// Notifications for a rename may arrive in either order.
	u.Reconcile(moved)
	u.Reconcile(filepath.Join(dir, "docs"))

	paths := livePaths(ix)
	for _, want := range []string{moved, filepath.Join(moved, "a.txt"), filepath.Join(moved, "sub", "b.txt"), filepath.Join(dir, "top.txt")} {
		if _, ok := paths[want]; !ok {
			t.Errorf("missing %s", want)
		}
	}
	for _, gone := range []string{filepath.Join(dir, "docs"), filepath.Join(dir, "docs", "sub", "b.txt")} {
		if _, ok := paths[gone]; ok {
			t.Errorf("stale %s", gone)
		}
	}
	if ix.Len() != len(paths) || ix.Len() != 6 {
		t.Errorf("live count %d, paths %d", ix.Len(), len(paths))
	}
}

func TestReconcileHonoursExcludesAndRoots(t *testing.T) {
	dir, ix, u := setup(t)
	tmp := filepath.Join(dir, "scratch.tmp")
	write(t, tmp, 1)
	if u.Reconcile(tmp) {
		t.Error("excluded file should be ignored")
	}
	if u.Reconcile(filepath.Join(t.TempDir(), "elsewhere.txt")) {
		t.Error("paths outside the roots should be ignored")
	}
	if _, ok := livePaths(ix)[tmp]; ok {
		t.Error("excluded file was indexed")
	}
}

func TestDeepPathBeforeParentIsKnown(t *testing.T) {
	dir, ix, u := setup(t)
	deep := filepath.Join(dir, "x", "y", "z.txt")
	write(t, deep, 1)
	if !u.Reconcile(deep) {
		t.Fatal("expected change")
	}
	paths := livePaths(ix)
	for _, want := range []string{filepath.Join(dir, "x"), filepath.Join(dir, "x", "y"), deep} {
		if _, ok := paths[want]; !ok {
			t.Errorf("missing %s", want)
		}
	}
}

func TestSurvivesCompaction(t *testing.T) {
	dir, ix, u := setup(t)
	os.Remove(filepath.Join(dir, "top.txt"))
	u.Reconcile(filepath.Join(dir, "top.txt"))
	ix.Compact()
	u.Rebuild()
	added := filepath.Join(dir, "docs", "sub", "c.txt")
	write(t, added, 1)
	if !u.Reconcile(added) {
		t.Fatal("expected change")
	}
	if _, ok := livePaths(ix)[added]; !ok {
		t.Error("file added after compaction not found")
	}
}

func write(t *testing.T, path string, size int) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, make([]byte, size), 0o644); err != nil {
		t.Fatal(err)
	}
}
