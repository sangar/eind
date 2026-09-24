// Package update applies filesystem changes to a loaded index.
//
// Change notifications are unreliable in order and detail, so every changed
// path is simply re-examined on disk: whatever exists is upserted, whatever
// is gone is removed, and a directory that appears is scanned in full.
package update

import (
	"errors"
	"os"
	"path/filepath"
	"strings"

	"eind/internal/index"
)

type Updater struct {
	ix       *index.Index
	ex       *index.Excludes
	dirs     map[string]uint32   // directory path -> entry
	children map[uint32][]uint32 // directory entry -> child entries
}

func New(ix *index.Index, ex *index.Excludes) *Updater {
	u := &Updater{ix: ix, ex: ex}
	u.Rebuild()
	return u
}

// Rebuild recreates the lookup tables; required after the index is compacted.
func (u *Updater) Rebuild() {
	u.dirs = make(map[string]uint32)
	u.children = make(map[uint32][]uint32)
	u.absorb(0, uint32(len(u.ix.Entries)))
}

func (u *Updater) absorb(start, end uint32) {
	for i := start; i < end; i++ {
		e := &u.ix.Entries[i]
		if e.Parent == index.Tombstone {
			continue
		}
		if e.Parent != index.NoParent {
			u.children[e.Parent] = append(u.children[e.Parent], i)
		}
		if e.IsDir {
			u.dirs[u.ix.Path(i)] = i
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

func (u *Updater) lookup(path string) (uint32, bool) {
	if i, ok := u.dirs[path]; ok {
		return i, true
	}
	parent, ok := u.dirs[filepath.Dir(path)]
	if !ok {
		return 0, false
	}
	return u.findChild(parent, filepath.Base(path))
}

func (u *Updater) findChild(parent uint32, name string) (uint32, bool) {
	for _, c := range u.children[parent] {
		if u.ix.Entries[c].Name == name {
			return c, true
		}
	}
	return 0, false
}

func (u *Updater) upsert(path string, info os.FileInfo) bool {
	if i, ok := u.lookup(path); ok {
		if u.ix.Entries[i].IsDir == info.IsDir() {
			return u.refresh(i, info)
		}
		u.remove(i)
	}
	parentPath := filepath.Dir(path)
	parent, ok := u.dirs[parentPath]
	if !ok {
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
	fresh := index.EntryFromInfo(u.ix.Entries[i].Name, u.ix.Entries[i].Parent, info)
	if fresh == u.ix.Entries[i] {
		return false
	}
	u.ix.Entries[i] = fresh
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

// remove tombstones an entry and all its descendants and forgets them in the
// lookup tables.
func (u *Updater) remove(i uint32) {
	parent := u.ix.Entries[i].Parent
	if kids, ok := u.children[parent]; ok {
		for k, c := range kids {
			if c == i {
				u.children[parent] = append(kids[:k], kids[k+1:]...)
				break
			}
		}
	}
	// Paths of descendants need their ancestors' names, so tombstone only
	// after the whole subtree has been collected.
	var subtree []uint32
	stack := []uint32{i}
	for len(stack) > 0 {
		cur := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		subtree = append(subtree, cur)
		if u.ix.Entries[cur].IsDir {
			delete(u.dirs, u.ix.Path(cur))
			stack = append(stack, u.children[cur]...)
			delete(u.children, cur)
		}
	}
	for _, cur := range subtree {
		u.ix.Remove(cur)
	}
}
