// Package index holds the file index and its on-disk format.
//
// Entries form a tree: every entry stores the id of its parent directory, and
// a parent always has a smaller id than its children. Full paths are rebuilt
// on demand by walking up the parent chain.
//
// An index is an immutable base, memory-mapped from the index file, plus an
// overlay of changes made since: added entries, removed entries and updated
// metadata. A running daemon appends every change to a journal next to the
// index file, so that other processes loading the index see it at once. Save
// folds the overlay into a new base.
package index

import (
	"errors"
	"math"
	"os"
	"strings"
	"time"
	"unicode/utf8"
	"unsafe"
)

const (
	// NoParent marks a root entry. Its Name is the absolute root path.
	NoParent uint32 = math.MaxUint32
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
	Roots   []string
	BuiltAt time.Time

	base       base
	added      []Entry // entries with ids from base.count on
	dead       bitset  // removed entries, over all ids
	deadCount  int
	updated    bitset           // base entries whose metadata is in changes
	changes    map[uint32]Entry // only Size, Modified and Created are used
	journal    *journal         // nil unless changes are being journaled
	generation uint64           // of the base; a journal belongs to one base
}

func New(roots []string) *Index {
	return &Index{Roots: roots, BuiltAt: time.Now()}
}

// Count is one more than the largest entry id, live or not.
func (ix *Index) Count() int { return ix.base.count + len(ix.added) }

// BaseCount is the number of entries in the base. Ids below it are in path
// order; entries added since have larger ids.
func (ix *Index) BaseCount() int { return ix.base.count }

// Len is the number of live entries.
func (ix *Index) Len() int { return ix.Count() - ix.deadCount }

func (ix *Index) Live(i uint32) bool { return !ix.dead.has(i) }

func (ix *Index) inBase(i uint32) bool { return int(i) < ix.base.count }

func (ix *Index) addedEntry(i uint32) *Entry { return &ix.added[int(i)-ix.base.count] }

func (ix *Index) Name(i uint32) string {
	if ix.inBase(i) {
		return ix.base.name(ix.base.nameID[i])
	}
	return ix.addedEntry(i).Name
}

func (ix *Index) Parent(i uint32) uint32 {
	if ix.inBase(i) {
		return ix.base.parent[i]
	}
	return ix.addedEntry(i).Parent
}

func (ix *Index) IsDir(i uint32) bool {
	if ix.inBase(i) {
		return ix.base.dir.has(i)
	}
	return ix.addedEntry(i).IsDir
}

func (ix *Index) Size(i uint32) int64 {
	switch {
	case !ix.inBase(i):
		return ix.addedEntry(i).Size
	case ix.updated.has(i):
		return ix.changes[i].Size
	}
	return ix.base.size[i]
}

func (ix *Index) Modified(i uint32) int64 {
	switch {
	case !ix.inBase(i):
		return ix.addedEntry(i).Modified
	case ix.updated.has(i):
		return ix.changes[i].Modified
	}
	return int64(ix.base.modified[i])
}

func (ix *Index) Created(i uint32) int64 {
	switch {
	case !ix.inBase(i):
		return ix.addedEntry(i).Created
	case ix.updated.has(i):
		return ix.changes[i].Created
	}
	return int64(ix.base.created[i])
}

func (ix *Index) Entry(i uint32) Entry {
	return Entry{Name: ix.Name(i), Parent: ix.Parent(i), Size: ix.Size(i), Modified: ix.Modified(i), Created: ix.Created(i), IsDir: ix.IsDir(i)}
}

// NameID returns the id of entry i's name among the base's distinct names,
// which are numbered in name order. Entries added since the base was written
// have none.
func (ix *Index) NameID(i uint32) (uint32, bool) {
	if ix.inBase(i) {
		return ix.base.nameID[i], true
	}
	return 0, false
}

// DistinctNames is the number of distinct names in the base.
func (ix *Index) DistinctNames() int { return len(ix.base.nameOff) }

// DistinctName returns the base name with the given id.
func (ix *Index) DistinctName(id uint32) string { return ix.base.name(id) }

func (ix *Index) Add(e Entry) uint32 {
	ix.added = append(ix.added, e)
	i := uint32(ix.Count() - 1)
	ix.journal.add(e)
	return i
}

// Remove marks a single entry as removed. Callers removing a directory must
// also remove its descendants; Save drops orphaned descendants regardless.
func (ix *Index) Remove(i uint32) {
	if ix.dead.has(i) {
		return
	}
	ix.dead.set(i)
	ix.deadCount++
	ix.journal.remove(i)
}

// Update replaces the size and times of entry i.
func (ix *Index) Update(i uint32, size, modified, created int64) {
	if !ix.inBase(i) {
		e := ix.addedEntry(i)
		e.Size, e.Modified, e.Created = size, modified, created
	} else {
		if ix.changes == nil {
			ix.changes = make(map[uint32]Entry)
		}
		ix.changes[i] = Entry{Size: size, Modified: modified, Created: created}
		ix.updated.set(i)
	}
	ix.journal.update(i, size, modified, created)
}

// Path reconstructs the absolute path of entry i.
func (ix *Index) Path(i uint32) string {
	var buf [64]string
	parts := buf[:0]
	size := 0
	for {
		name := ix.Name(i)
		parts = append(parts, name)
		size += len(name) + 1
		p := ix.Parent(i)
		if p == NoParent {
			break
		}
		i = p
	}
	var b strings.Builder
	b.Grow(size)
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

// Lower lowercases a name, without allocating for the common case of a name
// that is already lowercase ASCII.
func Lower(name string) string {
	for k := 0; k < len(name); k++ {
		if c := name[k]; c >= utf8.RuneSelf || 'A' <= c && c <= 'Z' {
			return strings.ToLower(name)
		}
	}
	return name
}

// LowerInto is Lower for hot loops: an ASCII name with uppercase letters is
// lowercased into scratch, and the result is only valid until scratch is
// reused.
func LowerInto(scratch *[]byte, name string) string {
	upper := false
	for k := 0; k < len(name); k++ {
		c := name[k]
		if c >= utf8.RuneSelf {
			return strings.ToLower(name)
		}
		upper = upper || 'A' <= c && c <= 'Z'
	}
	if !upper {
		return name
	}
	b := append((*scratch)[:0], name...)
	for k, c := range b {
		if 'A' <= c && c <= 'Z' {
			b[k] = c + 'a' - 'A'
		}
	}
	*scratch = b
	return unsafe.String(unsafe.SliceData(b), len(b))
}

// Ext returns the lowercased extension of a name without the dot, or "".
func Ext(name string) string {
	dot := strings.LastIndexByte(name, '.')
	if dot <= 0 {
		return ""
	}
	return Lower(name[dot+1:])
}

// Stats counts live files and directories.
func (ix *Index) Stats() (files, dirs int) {
	for i := range uint32(ix.Count()) {
		if !ix.Live(i) {
			continue
		}
		if ix.IsDir(i) {
			dirs++
		} else {
			files++
		}
	}
	return files, dirs
}

var ErrNotFound = errors.New("index file not found")

// bitset grows as bits are set; reading past its end reports false.
type bitset []uint64

func (b bitset) has(i uint32) bool {
	w := int(i / 64)
	return w < len(b) && b[w]&(1<<(i%64)) != 0
}

func (b *bitset) set(i uint32) {
	w := int(i / 64)
	if w >= len(*b) {
		*b = append(*b, make([]uint64, w+1-len(*b))...)
	}
	(*b)[w] |= 1 << (i % 64)
}
