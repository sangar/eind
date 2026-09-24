// Package index holds the in-memory file index and its on-disk format.
//
// Entries form a tree: every entry stores the index of its parent directory,
// and a parent is always stored before its children. Full paths are
// reconstructed on demand by walking up the parent chain, which keeps the
// index small enough to hold millions of files in memory.
package index

import (
	"errors"
	"fmt"
	"math"
	"os"
	"runtime"
	"strings"
	"sync"
	"time"
)

const (
	// NoParent marks a root entry. Its Name is the absolute root path.
	NoParent uint32 = math.MaxUint32
	// Tombstone marks an entry that has been removed but not yet compacted away.
	Tombstone uint32 = math.MaxUint32 - 1
)

type Entry struct {
	Name     string
	Parent   uint32
	Size     int64
	Modified int64 // unix seconds
	Created  int64 // unix seconds, 0 when the platform does not report it
	IsDir    bool
}

type Index struct {
	Entries []Entry
	// Lower holds the lowercased name of every entry, for case-insensitive search.
	Lower      []string
	Roots      []string
	BuiltAt    time.Time
	tombstones int
}

func New(roots []string) *Index {
	return &Index{Roots: roots, BuiltAt: time.Now()}
}

// Len is the number of live entries.
func (ix *Index) Len() int { return len(ix.Entries) - ix.tombstones }

func (ix *Index) Live(i uint32) bool { return ix.Entries[i].Parent != Tombstone }

func (ix *Index) Add(e Entry) uint32 {
	ix.Entries = append(ix.Entries, e)
	ix.Lower = append(ix.Lower, strings.ToLower(e.Name))
	return uint32(len(ix.Entries) - 1)
}

// Remove tombstones a single entry. Callers removing a directory must also
// remove its descendants; Compact drops orphaned descendants regardless.
func (ix *Index) Remove(i uint32) {
	if ix.Entries[i].Parent == Tombstone {
		return
	}
	ix.Entries[i].Parent = Tombstone
	ix.Entries[i].Name = ""
	ix.Lower[i] = ""
	ix.tombstones++
}

// Path reconstructs the absolute path of entry i.
func (ix *Index) Path(i uint32) string {
	var parts [64]string
	n := 0
	size := 0
	for {
		e := &ix.Entries[i]
		if n < len(parts) {
			parts[n] = e.Name
		} else {
			return ix.deepPath(i)
		}
		n++
		size += len(e.Name) + 1
		if e.Parent == NoParent {
			break
		}
		i = e.Parent
	}
	var b strings.Builder
	b.Grow(size)
	for k := n - 1; k >= 0; k-- {
		joinComponent(&b, parts[k])
	}
	return b.String()
}

func (ix *Index) deepPath(i uint32) string {
	var parts []string
	for {
		e := &ix.Entries[i]
		parts = append(parts, e.Name)
		if e.Parent == NoParent {
			break
		}
		i = e.Parent
	}
	var b strings.Builder
	for k := len(parts) - 1; k >= 0; k-- {
		joinComponent(&b, parts[k])
	}
	return b.String()
}

func joinComponent(b *strings.Builder, part string) {
	if b.Len() > 0 && !strings.HasSuffix(b.String(), string(os.PathSeparator)) {
		b.WriteByte(os.PathSeparator)
	}
	b.WriteString(part)
}

// Ext returns the lowercased extension of entry i without the dot, or "".
func (ix *Index) Ext(i uint32) string {
	name := ix.Lower[i]
	dot := strings.LastIndexByte(name, '.')
	if dot <= 0 {
		return ""
	}
	return name[dot+1:]
}

// Stats counts live files and directories.
func (ix *Index) Stats() (files, dirs int) {
	for i := range ix.Entries {
		e := &ix.Entries[i]
		if e.Parent == Tombstone {
			continue
		}
		if e.IsDir {
			dirs++
		} else {
			files++
		}
	}
	return files, dirs
}

// Compact drops tombstoned entries and everything below them, renumbering
// parent references. Entry indices are not stable across a Compact.
func (ix *Index) Compact() {
	if ix.tombstones == 0 {
		return
	}
	remap := make([]uint32, len(ix.Entries))
	kept := make([]Entry, 0, len(ix.Entries)-ix.tombstones)
	lower := make([]string, 0, len(ix.Entries)-ix.tombstones)
	for i, e := range ix.Entries {
		dead := e.Parent == Tombstone || (e.Parent != NoParent && remap[e.Parent] == Tombstone)
		if dead {
			remap[i] = Tombstone
			continue
		}
		if e.Parent != NoParent {
			e.Parent = remap[e.Parent]
		}
		remap[i] = uint32(len(kept))
		kept = append(kept, e)
		lower = append(lower, ix.Lower[i])
	}
	ix.Entries = kept
	ix.Lower = lower
	ix.tombstones = 0
}

var ErrNotFound = errors.New("index file not found")

func Load(path string) (*Index, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil, ErrNotFound
		}
		return nil, err
	}
	ix, err := decode(data)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	ix.fillLower()
	return ix, nil
}

func (ix *Index) fillLower() {
	ix.Lower = make([]string, len(ix.Entries))
	workers := runtime.GOMAXPROCS(0)
	chunk := (len(ix.Entries) + workers - 1) / workers
	var wg sync.WaitGroup
	for lo := 0; lo < len(ix.Entries); lo += chunk {
		hi := min(lo+chunk, len(ix.Entries))
		wg.Add(1)
		go func(lo, hi int) {
			defer wg.Done()
			for i := lo; i < hi; i++ {
				ix.Lower[i] = strings.ToLower(ix.Entries[i].Name)
			}
		}(lo, hi)
	}
	wg.Wait()
}

// Save compacts the index and writes it atomically.
func (ix *Index) Save(path string) error {
	ix.Compact()
	if err := os.MkdirAll(dirOf(path), 0o755); err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, encode(ix), 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func dirOf(path string) string {
	if i := strings.LastIndexByte(path, os.PathSeparator); i > 0 {
		return path[:i]
	}
	return "."
}
