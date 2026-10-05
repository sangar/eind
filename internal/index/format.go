package index

import (
	"cmp"
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"os"
	"slices"
	"strings"
	"time"
	"unsafe"
)

// The index file is a header followed by 8-byte aligned sections, laid out so
// that the columns are used straight from the mapped file:
//
//	roots      root paths, each followed by NUL
//	names      the distinct names in name order, each followed by NUL
//	name offs  u32 per distinct name: its offset in names
//	name ids   u32 per entry
//	parents    u32 per entry
//	sizes      i64 per entry
//	modified   u32 per entry, unix seconds
//	created    u32 per entry, unix seconds, 0 when unknown
//	dirs       a bitset over entries
//	unicode    a bitset over distinct names whose Unicode lowercasing differs
//	           from ASCII lowercasing, see NameCandidates
//
// Entries are stored in the order of their lowercased paths, so a path sort of
// base entries is a sort by id, and names are numbered in name order, so a
// name sort is a sort by name id. All numbers are little-endian, which is
// every platform eind builds for.
const (
	magic         = "EIND"
	formatVersion = 2
	headerSize    = 32 + 16*sectionCount
	sectionCount  = 10
)

type header struct {
	version    uint32
	generation uint64
	builtAt    int64
	count      uint32
	distinct   uint32
	offsets    [sectionCount]uint64
	lengths    [sectionCount]uint64
}

// base is the immutable part of an index, mapped from the index file.
type base struct {
	data     []byte
	release  func() error
	count    int
	names    []byte
	nameOff  []uint32
	nameID   []uint32
	parent   []uint32
	size     []int64
	modified []uint32
	created  []uint32
	dir      bitset
	unicode  bitset
}

func (b *base) name(id uint32) string {
	start := b.nameOff[id]
	end := uint32(len(b.names)) - 1
	if int(id)+1 < len(b.nameOff) {
		end = b.nameOff[id+1] - 1
	}
	return unsafe.String(unsafe.SliceData(b.names[start:]), end-start)
}

var errCorrupt = errors.New("corrupt index file")

func Load(path string) (*Index, error) {
	ix, err := loadBase(path)
	if err != nil {
		return nil, err
	}
	if err := ix.replayJournal(path); err != nil {
		ix.close()
		return nil, err
	}
	return ix, nil
}

// Reload replaces the index with the one at path, for when another process
// wrote it. Entry ids change, and journaling stops until EnableJournal.
func (ix *Index) Reload(path string) error {
	fresh, err := Load(path)
	if err != nil {
		return err
	}
	ix.close()
	*ix = *fresh
	return nil
}

func loadBase(path string) (*Index, error) {
	data, release, err := mapFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	ix, err := decode(data)
	if err != nil {
		release()
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	ix.base.release = release
	return ix, nil
}

// close releases the mapped base. Names returned by the index must not be
// used afterwards.
func (ix *Index) close() {
	if ix.base.release != nil {
		ix.base.release()
	}
	ix.base = base{}
}

func decode(data []byte) (*Index, error) {
	if len(data) < headerSize || string(data[:4]) != magic {
		return nil, errCorrupt
	}
	le := binary.LittleEndian
	h := header{version: le.Uint32(data[4:])}
	if h.version != formatVersion {
		return nil, errors.New("index was written by an incompatible eind version; run `eind index`")
	}
	h.generation = le.Uint64(data[8:])
	h.builtAt = int64(le.Uint64(data[16:]))
	h.count = le.Uint32(data[24:])
	h.distinct = le.Uint32(data[28:])
	for k := range sectionCount {
		h.offsets[k] = le.Uint64(data[32+16*k:])
		h.lengths[k] = le.Uint64(data[40+16*k:])
	}
	section := func(k int, elem, n uint64) ([]byte, bool) {
		off, length := h.offsets[k], h.lengths[k]
		ok := off%8 == 0 && off <= uint64(len(data)) && length <= uint64(len(data))-off && (elem == 0 || length == elem*n)
		if !ok {
			return nil, false
		}
		return data[off : off+length], true
	}
	n, d := uint64(h.count), uint64(h.distinct)
	roots, ok1 := section(0, 0, 0)
	names, ok2 := section(1, 0, 0)
	nameOff, ok3 := section(2, 4, d)
	nameID, ok4 := section(3, 4, n)
	parent, ok5 := section(4, 4, n)
	size, ok6 := section(5, 8, n)
	modified, ok7 := section(6, 4, n)
	created, ok8 := section(7, 4, n)
	dir, ok9 := section(8, 8, (n+63)/64)
	unicode, ok10 := section(9, 8, (d+63)/64)
	if !(ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7 && ok8 && ok9 && ok10) {
		return nil, errCorrupt
	}
	ix := &Index{
		BuiltAt:    time.Unix(h.builtAt, 0),
		generation: h.generation,
		base: base{
			data:     data,
			count:    int(n),
			names:    names,
			nameOff:  column[uint32](nameOff),
			nameID:   column[uint32](nameID),
			parent:   column[uint32](parent),
			size:     column[int64](size),
			modified: column[uint32](modified),
			created:  column[uint32](created),
			dir:      column[uint64](dir),
			unicode:  column[uint64](unicode),
		},
	}
	for r := range strings.SplitSeq(strings.TrimSuffix(string(roots), "\x00"), "\x00") {
		if r != "" {
			ix.Roots = append(ix.Roots, r)
		}
	}
	if err := ix.base.validate(); err != nil {
		return nil, err
	}
	return ix, nil
}

func column[T any](b []byte) []T {
	var zero T
	if len(b) == 0 {
		return nil
	}
	return unsafe.Slice((*T)(unsafe.Pointer(unsafe.SliceData(b))), len(b)/int(unsafe.Sizeof(zero)))
}

// validate checks every reference the accessors follow, so that a damaged
// file is reported instead of crashing a search.
func (b *base) validate() error {
	if len(b.nameOff) > 0 && (len(b.names) == 0 || b.names[len(b.names)-1] != 0) {
		return errCorrupt
	}
	for k, off := range b.nameOff {
		if int(off) >= len(b.names) || k > 0 && off <= b.nameOff[k-1] {
			return errCorrupt
		}
	}
	distinct := uint32(len(b.nameOff))
	for i := range b.count {
		if b.nameID[i] >= distinct || b.parent[i] != NoParent && b.parent[i] >= uint32(i) {
			return errCorrupt
		}
	}
	return nil
}

// Save writes the live entries as a new base and starts an empty journal for
// it, then continues from the new base. Entry ids are not stable across a
// Save.
func (ix *Index) Save(path string) error {
	entries := ix.liveEntries()
	if err := os.MkdirAll(dirOf(path), 0o755); err != nil {
		return err
	}
	generation := uint64(time.Now().UnixNano())
	data := encode(entries, ix.Roots, ix.BuiltAt, generation)
	// The new journal goes in first and the new base last: a reader that
	// sees a journal from another generation ignores it, and a journal is
	// always folded into any base newer than it.
	if err := writeFileAtomic(journalPath(path), journalHeader(generation)); err != nil {
		return err
	}
	if err := writeFileAtomic(path, data); err != nil {
		return err
	}
	fresh, err := loadBase(path)
	if err != nil {
		return err
	}
	journaling := ix.journal != nil
	ix.close()
	*ix = *fresh
	if journaling {
		return ix.EnableJournal(path)
	}
	return nil
}

// liveEntries returns the live entries, dropping the orphans of removed
// directories and renumbering parents. Names from the base are copied, as the
// base is unmapped once a new one is written.
func (ix *Index) liveEntries() []Entry {
	if ix.base.count == 0 && ix.deadCount == 0 {
		return ix.added
	}
	n := ix.Count()
	remap := make([]uint32, n)
	entries := make([]Entry, 0, ix.Len())
	for i := range uint32(n) {
		p := ix.Parent(i)
		if !ix.Live(i) || p != NoParent && remap[p] == NoParent {
			remap[i] = NoParent
			continue
		}
		e := ix.Entry(i)
		if ix.inBase(i) {
			e.Name = strings.Clone(e.Name)
		}
		if p != NoParent {
			e.Parent = remap[p]
		}
		remap[i] = uint32(len(entries))
		entries = append(entries, e)
	}
	return entries
}

func writeFileAtomic(path string, data []byte) error {
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func encode(entries []Entry, roots []string, builtAt time.Time, generation uint64) []byte {
	order, newID := pathOrder(entries)
	names, nameOff, nameIDs, unicode := distinctNames(entries)
	n := len(entries)
	ids := make([]uint32, n)
	parent := make([]uint32, n)
	size := make([]int64, n)
	modified, created := make([]uint32, n), make([]uint32, n)
	dir := make(bitset, (n+63)/64)
	for i, old := range order {
		e := &entries[old]
		ids[i], size[i] = nameIDs[old], e.Size
		parent[i] = NoParent
		if e.Parent != NoParent {
			parent[i] = newID[e.Parent]
		}
		modified[i], created[i] = clampTime(e.Modified), clampTime(e.Created)
		if e.IsDir {
			dir.set(uint32(i))
		}
	}
	var rootBytes []byte
	for _, r := range roots {
		rootBytes = append(append(rootBytes, r...), 0)
	}
	sections := [sectionCount][]byte{rootBytes, names, bytesOf(nameOff), bytesOf(ids), bytesOf(parent), bytesOf(size), bytesOf(modified), bytesOf(created), bytesOf(dir), bytesOf(unicode)}

	le := binary.LittleEndian
	out := make([]byte, headerSize)
	copy(out, magic)
	le.PutUint32(out[4:], formatVersion)
	le.PutUint64(out[8:], generation)
	le.PutUint64(out[16:], uint64(builtAt.Unix()))
	le.PutUint32(out[24:], uint32(n))
	le.PutUint32(out[28:], uint32(len(nameOff)))
	for k, s := range sections {
		for len(out)%8 != 0 {
			out = append(out, 0)
		}
		le.PutUint64(out[32+16*k:], uint64(len(out)))
		le.PutUint64(out[40+16*k:], uint64(len(s)))
		out = append(out, s...)
	}
	return out
}

// clampTime fits unix seconds in 32 bits, which holds dates from 1970 to 2106.
func clampTime(t int64) uint32 {
	return uint32(min(max(t, 0), math.MaxUint32))
}

func bytesOf[T any](s []T) []byte {
	var zero T
	return unsafe.Slice((*byte)(unsafe.Pointer(unsafe.SliceData(s))), len(s)*int(unsafe.Sizeof(zero)))
}

// distinctNames stores every name once, numbered in name order: lowercased,
// then as written. It returns each entry's name id, indexed like entries.
func distinctNames(entries []Entry) (names []byte, offsets, ids []uint32, unicode bitset) {
	idOf := make(map[string]uint32, len(entries)/2)
	var distinct []string
	ids = make([]uint32, len(entries))
	for i, e := range entries {
		id, ok := idOf[e.Name]
		if !ok {
			id = uint32(len(distinct))
			idOf[e.Name] = id
			distinct = append(distinct, e.Name)
		}
		ids[i] = id
	}
	lower := make([]string, len(distinct))
	for k, name := range distinct {
		lower[k] = Lower(name)
	}
	byName := make([]uint32, len(distinct))
	for k := range byName {
		byName[k] = uint32(k)
	}
	slices.SortFunc(byName, func(a, b uint32) int {
		return cmp.Or(strings.Compare(lower[a], lower[b]), strings.Compare(distinct[a], distinct[b]))
	})
	rank := make([]uint32, len(distinct))
	offsets = make([]uint32, len(distinct))
	unicode = make(bitset, (len(distinct)+63)/64)
	for id, k := range byName {
		name := distinct[k]
		rank[k] = uint32(id)
		offsets[id] = uint32(len(names))
		names = append(append(names, name...), 0)
		if foldsBeyondASCII(name) {
			unicode.set(uint32(id))
		}
	}
	for i, k := range ids {
		ids[i] = rank[k]
	}
	return names, offsets, ids, unicode
}

// pathOrder orders entries by lowercased path, then name, as SortPath does,
// keeping parents before children. It returns the entries in that order and
// each entry's new position. It walks the tree with each directory's children
// sorted by name, where a directory appears twice: once as itself, keyed by
// its name, and once as the subtree below it, keyed by its name and a
// separator. That places "a.txt" between "a" and "a/b", as comparing whole
// paths would.
func pathOrder(entries []Entry) (order, newID []uint32) {
	type item struct {
		key     string
		entry   uint32
		subtree bool
	}
	// Children of each directory, grouped in one array; the roots are listed
	// under a virtual directory numbered len(entries).
	n := len(entries)
	start := make([]uint32, n+2)
	for _, e := range entries {
		p := e.Parent
		if p == NoParent {
			p = uint32(n)
		}
		start[p+1]++
		if e.IsDir {
			start[p+1]++
		}
	}
	for k := 1; k < len(start); k++ {
		start[k] += start[k-1]
	}
	items := make([]item, start[n+1])
	fill := slices.Clone(start)
	sep := string(os.PathSeparator)
	for i, e := range entries {
		p := e.Parent
		if p == NoParent {
			p = uint32(n)
		}
		key := Lower(e.Name)
		items[fill[p]] = item{key: key, entry: uint32(i)}
		fill[p]++
		if e.IsDir {
			items[fill[p]] = item{key: strings.TrimSuffix(key, sep) + sep, entry: uint32(i), subtree: true}
			fill[p]++
		}
	}
	for p := range n + 1 {
		slices.SortFunc(items[start[p]:start[p+1]], func(a, b item) int {
			return cmp.Or(strings.Compare(a.key, b.key), strings.Compare(entries[a.entry].Name, entries[b.entry].Name),
				cmp.Compare(a.entry, b.entry), compareBool(a.subtree, b.subtree))
		})
	}
	order = make([]uint32, 0, n)
	newID = make([]uint32, n)
	stack := []item{{entry: uint32(n), subtree: true}}
	for len(stack) > 0 {
		it := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		if !it.subtree {
			newID[it.entry] = uint32(len(order))
			order = append(order, it.entry)
			continue
		}
		kids := items[start[it.entry]:start[it.entry+1]]
		for k := len(kids) - 1; k >= 0; k-- {
			stack = append(stack, kids[k])
		}
	}
	return order, newID
}

func compareBool(a, b bool) int {
	switch {
	case a == b:
		return 0
	case a:
		return 1
	}
	return -1
}

func dirOf(path string) string {
	if i := strings.LastIndexByte(path, os.PathSeparator); i > 0 {
		return path[:i]
	}
	return "."
}
