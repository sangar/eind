package index

import (
	"encoding/binary"
	"errors"
	"io"
	"os"
)

// The journal lists the changes made since its base was written, so that any
// process loading the index sees them without waiting for a new base. It is
// a header naming the base's generation, then entries:
//
//	'A' parent u32, size i64, modified i64, created i64, dir u8, name length u16, name
//	'R' id u32
//	'U' id u32, size i64, modified i64, created i64
//
// Ids are as the base numbers them, with added entries numbered after it in
// journal order. The writer appends whole batches; a reader that finds a
// half-written last entry ignores it.
const (
	journalMagic      = "EINJ"
	journalHeaderSize = 12
	opAdd             = 'A'
	opRemove          = 'R'
	opUpdate          = 'U'
)

// ErrReplaced reports that another process wrote a new index file, so this
// one's journal no longer matches it and the index must be loaded again.
var ErrReplaced = errors.New("the index file was replaced by another process")

func journalPath(indexPath string) string { return indexPath + ".journal" }

func journalHeader(generation uint64) []byte {
	return binary.LittleEndian.AppendUint64([]byte(journalMagic), generation)
}

type journal struct {
	path    string
	header  []byte
	pending []byte
	entries int // since the base was written
}

// EnableJournal makes every later change be recorded in the journal of the
// index file at indexPath once Flush is called. A journal that is missing,
// belongs to another base, or ends in a half-written entry is rewritten to
// hold exactly the changes this index has replayed.
func (ix *Index) EnableJournal(indexPath string) error {
	j := &journal{path: journalPath(indexPath), header: journalHeader(ix.generation)}
	data, err := os.ReadFile(j.path)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	valid, entries := ix.validJournal(data)
	if valid < 0 {
		data, valid, entries = j.header, len(j.header), 0
		if err := writeFileAtomic(j.path, data); err != nil {
			return err
		}
	}
	if valid < len(data) {
		if err := os.Truncate(j.path, int64(valid)); err != nil {
			return err
		}
	}
	j.entries = entries
	ix.journal = j
	return nil
}

// validJournal returns the length of the whole entries in data and their
// count, or -1 if data is not this base's journal.
func (ix *Index) validJournal(data []byte) (int, int) {
	header := journalHeader(ix.generation)
	if len(data) < journalHeaderSize || string(data[:journalHeaderSize]) != string(header) {
		return -1, 0
	}
	p, n := journalHeaderSize, 0
	for p < len(data) {
		size := entrySize(data[p:])
		if size == 0 {
			break
		}
		p += size
		n++
	}
	return p, n
}

// entrySize is the length of the entry at the start of b, or 0 if it is
// incomplete or unknown.
func entrySize(b []byte) int {
	need := 0
	switch b[0] {
	case opAdd:
		if len(b) < 32 {
			return 0
		}
		need = 32 + int(binary.LittleEndian.Uint16(b[30:]))
	case opRemove:
		need = 5
	case opUpdate:
		need = 29
	default:
		return 0
	}
	if len(b) < need {
		return 0
	}
	return need
}

// replayJournal applies the journal of the index file at indexPath. A
// journal of another generation is ignored: either it predates the base and
// is already folded into it, or a new base is being written and its journal
// is still empty.
func (ix *Index) replayJournal(indexPath string) error {
	data, err := os.ReadFile(journalPath(indexPath))
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	valid, _ := ix.validJournal(data)
	if valid < 0 {
		return nil
	}
	le := binary.LittleEndian
	for p := journalHeaderSize; p < valid; p += entrySize(data[p:]) {
		b := data[p:]
		switch b[0] {
		case opAdd:
			e := Entry{
				Parent:   le.Uint32(b[1:]),
				Size:     int64(le.Uint64(b[5:])),
				Modified: int64(le.Uint64(b[13:])),
				Created:  int64(le.Uint64(b[21:])),
				IsDir:    b[29] != 0,
				Name:     string(b[32 : 32+int(le.Uint16(b[30:]))]),
			}
			if e.Parent != NoParent && int(e.Parent) >= ix.Count() {
				return errCorrupt
			}
			ix.Add(e)
		case opRemove, opUpdate:
			id := le.Uint32(b[1:])
			if int(id) >= ix.Count() {
				return errCorrupt
			}
			if b[0] == opRemove {
				ix.Remove(id)
			} else {
				ix.Update(id, int64(le.Uint64(b[5:])), int64(le.Uint64(b[13:])), int64(le.Uint64(b[21:])))
			}
		}
	}
	return nil
}

// The journal methods do nothing until EnableJournal.

func (j *journal) add(e Entry) {
	if j == nil {
		return
	}
	le := binary.LittleEndian
	b := append(j.pending, opAdd)
	b = le.AppendUint32(b, e.Parent)
	b = le.AppendUint64(b, uint64(e.Size))
	b = le.AppendUint64(b, uint64(e.Modified))
	b = le.AppendUint64(b, uint64(e.Created))
	b = append(b, boolByte(e.IsDir))
	b = le.AppendUint16(b, uint16(len(e.Name)))
	j.pending = append(b, e.Name...)
	j.entries++
}

func (j *journal) remove(i uint32) {
	if j == nil {
		return
	}
	j.pending = binary.LittleEndian.AppendUint32(append(j.pending, opRemove), i)
	j.entries++
}

func (j *journal) update(i uint32, size, modified, created int64) {
	if j == nil {
		return
	}
	le := binary.LittleEndian
	b := le.AppendUint32(append(j.pending, opUpdate), i)
	b = le.AppendUint64(b, uint64(size))
	b = le.AppendUint64(b, uint64(modified))
	j.pending = le.AppendUint64(b, uint64(created))
	j.entries++
}

func boolByte(b bool) byte {
	if b {
		return 1
	}
	return 0
}

// Flush appends the changes made since the last Flush to the journal. It
// returns ErrReplaced if the index file now belongs to another base.
func (ix *Index) Flush() error {
	j := ix.journal
	if j == nil || len(j.pending) == 0 {
		return nil
	}
	f, err := os.OpenFile(j.path, os.O_RDWR|os.O_APPEND, 0)
	if err != nil {
		return err
	}
	defer f.Close()
	header := make([]byte, journalHeaderSize)
	if _, err := io.ReadFull(f, header); err != nil || string(header) != string(j.header) {
		return ErrReplaced
	}
	if _, err := f.Write(j.pending); err != nil {
		return err
	}
	j.pending = j.pending[:0]
	return nil
}

// JournalEntries is the number of changes since the base was written.
func (ix *Index) JournalEntries() int {
	if ix.journal == nil {
		return 0
	}
	return ix.journal.entries
}
