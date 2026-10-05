// Package update applies filesystem changes to a loaded index.
//
// Change notifications are unreliable in order and detail, so every changed
// path is simply re-examined on disk: whatever exists is upserted, whatever
// is gone is removed, and a directory that appears is scanned in full.
package update

import (
	"errors"
	"iter"
	"os"
	"path/filepath"
	"slices"
	"strings"

	"eind/internal/index"
)

// Updater finds entries by path by walking down from a root through each
// directory's children. Those are listed in two flat arrays rather than maps,
// which for a million entries keeps the lookup tables near 10 MB with
// nothing for the garbage collector to scan.
type Updater struct {
	ix *index.Index
	ex *index.Excludes
	// The children of entry i are kids[first[i]:first[i+1]], for the
	// entries that existed at the last Rebuild; later ones are in added.
	first []uint32
	kids  []uint32
	added map[uint32][]uint32
	roots []uint32
}

func New(ix *index.Index, ex *index.Excludes) *Updater {
	u := &Updater{ix: ix, ex: ex}
	u.Rebuild()
	return u
}

// Rebuild recreates the lookup tables; required after the index is saved
// or reloaded, which renumbers entries.
func (u *Updater) Rebuild() {
	n := u.ix.Count()
	u.first = make([]uint32, n+1)
	for i := range uint32(n) {
		if p := u.ix.Parent(i); p != index.NoParent {
			u.first[p+1]++
		}
	}
	for k := 1; k <= n; k++ {
		u.first[k] += u.first[k-1]
	}
	u.kids = make([]uint32, u.first[n])
	u.roots = nil
	fill := slices.Clone(u.first[:n])
	for i := range uint32(n) {
		if p := u.ix.Parent(i); p != index.NoParent {
			u.kids[fill[p]] = i
			fill[p]++
		} else if u.ix.Live(i) {
			u.roots = append(u.roots, i)
		}
	}
	u.added = make(map[uint32][]uint32)
}

func (u *Updater) absorb(start, end uint32) {
	for i := start; i < end; i++ {
		if p := u.ix.Parent(i); p != index.NoParent {
			u.added[p] = append(u.added[p], i)
		}
	}
}

// children lists the entries directly below i, removed ones included.
func (u *Updater) children(i uint32) iter.Seq[uint32] {
	return func(yield func(uint32) bool) {
		if int(i)+1 < len(u.first) {
			for _, c := range u.kids[u.first[i]:u.first[i+1]] {
				if !yield(c) {
					return
				}
			}
		}
		for _, c := range u.added[i] {
			if !yield(c) {
				return
			}
		}
	}
}

// Reconcile brings the index in line with the current state of path.
// It reports whether the index changed.
func (u *Updater) Reconcile(path string) bool {
	path = filepath.Clean(path)
	if !u.underRoot(path) {
		return false
	}
	name := filepath.Base(path)
	if u.ex.Match(path, name) {
		return u.removePath(path)
	}
	info, err := os.Lstat(path)
	if errors.Is(err, os.ErrNotExist) {
		return u.removePath(path)
	}
	if err != nil {
		return false
	}
	return u.upsert(path, info)
}

func (u *Updater) underRoot(path string) bool {
	for _, root := range u.ix.Roots {
		if path == root || strings.HasPrefix(path, strings.TrimSuffix(root, string(os.PathSeparator))+string(os.PathSeparator)) {
			return true
		}
	}
	return false
}

// lookup finds the live entry at path by walking down from its root.
func (u *Updater) lookup(path string) (uint32, bool) {
	sep := string(os.PathSeparator)
	for _, root := range u.roots {
		name := u.ix.Name(root)
		prefix := strings.TrimSuffix(name, sep)
		if path == name || path == prefix {
			return root, true
		}
		rest, ok := strings.CutPrefix(path, prefix+sep)
		if !ok {
			continue
		}
		cur := root
		for comp := range strings.SplitSeq(rest, sep) {
			if cur, ok = u.findChild(cur, comp); !ok {
				return 0, false
			}
		}
		return cur, true
	}
	return 0, false
}

func (u *Updater) findChild(parent uint32, name string) (uint32, bool) {
	for c := range u.children(parent) {
		if u.ix.Live(c) && u.ix.Name(c) == name {
			return c, true
		}
	}
	return 0, false
}

func (u *Updater) upsert(path string, info os.FileInfo) bool {
	if i, ok := u.lookup(path); ok {
		if u.ix.IsDir(i) == info.IsDir() {
			return u.refresh(i, info)
		}
		u.remove(i)
	}
	parentPath := filepath.Dir(path)
	parent, ok := u.lookup(parentPath)
	if !ok || !u.ix.IsDir(parent) {
		// The parent is not indexed yet (events arrive out of order);
		// reconciling it scans this path along with the rest of the subtree.
		return u.Reconcile(parentPath)
	}
	if info.IsDir() {
		res, err := u.ix.AddTree(path, parent, u.ex, nil)
		if err != nil {
			return false
		}
		u.absorb(res.Start, res.End)
		return res.End > res.Start
	}
	i := u.ix.Add(index.EntryFromInfo(filepath.Base(path), parent, info))
	u.absorb(i, i+1)
	return true
}

func (u *Updater) refresh(i uint32, info os.FileInfo) bool {
	fresh := index.EntryFromInfo(u.ix.Name(i), u.ix.Parent(i), info)
	if fresh == u.ix.Entry(i) {
		return false
	}
	u.ix.Update(i, fresh.Size, fresh.Modified, fresh.Created)
	return true
}

func (u *Updater) removePath(path string) bool {
	i, ok := u.lookup(path)
	if !ok {
		return false
	}
	u.remove(i)
	return true
}

// remove marks an entry and all its live descendants as removed.
func (u *Updater) remove(i uint32) {
	stack := []uint32{i}
	for len(stack) > 0 {
		cur := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		if !u.ix.Live(cur) {
			continue
		}
		u.ix.Remove(cur)
		if u.ix.IsDir(cur) {
			stack = slices.AppendSeq(stack, u.children(cur))
		}
	}
}
