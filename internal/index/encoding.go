package index

import (
	"encoding/binary"
	"errors"
	"time"
)

const (
	magic         = "EIND"
	formatVersion = 1
	flagDir       = 1
)

// The file is one contiguous byte string; on load, names are sliced straight
// out of it, so a million entries load without a million allocations.
func encode(ix *Index) []byte {
	buf := make([]byte, 0, 32*len(ix.Entries))
	buf = append(buf, magic...)
	buf = binary.LittleEndian.AppendUint32(buf, formatVersion)
	buf = binary.AppendVarint(buf, ix.BuiltAt.Unix())
	buf = binary.AppendUvarint(buf, uint64(len(ix.Roots)))
	for _, r := range ix.Roots {
		buf = appendString(buf, r)
	}
	buf = binary.AppendUvarint(buf, uint64(len(ix.Entries)))
	for i := range ix.Entries {
		e := &ix.Entries[i]
		buf = appendString(buf, e.Name)
		buf = binary.AppendUvarint(buf, uint64(e.Parent))
		buf = binary.AppendUvarint(buf, uint64(e.Size))
		buf = binary.AppendVarint(buf, e.Modified)
		buf = binary.AppendVarint(buf, e.Created)
		var flags byte
		if e.IsDir {
			flags |= flagDir
		}
		buf = append(buf, flags)
	}
	return buf
}

func appendString(buf []byte, s string) []byte {
	buf = binary.AppendUvarint(buf, uint64(len(s)))
	return append(buf, s...)
}

var errCorrupt = errors.New("corrupt index file")

type reader struct {
	buf  []byte
	data string // the same bytes as buf; names are sliced from it
	pos  int
	err  error
}

func (r *reader) uvarint() uint64 {
	if r.err != nil {
		return 0
	}
	v, n := binary.Uvarint(r.buf[r.pos:])
	if n <= 0 {
		r.err = errCorrupt
		return 0
	}
	r.pos += n
	return v
}

func (r *reader) varint() int64 {
	if r.err != nil {
		return 0
	}
	v, n := binary.Varint(r.buf[r.pos:])
	if n <= 0 {
		r.err = errCorrupt
		return 0
	}
	r.pos += n
	return v
}

func (r *reader) str() string {
	n := int(r.uvarint())
	if r.err != nil || r.pos+n > len(r.data) {
		r.err = errCorrupt
		return ""
	}
	s := r.data[r.pos : r.pos+n]
	r.pos += n
	return s
}

func (r *reader) byte() byte {
	if r.err != nil || r.pos >= len(r.data) {
		r.err = errCorrupt
		return 0
	}
	b := r.data[r.pos]
	r.pos++
	return b
}

func decode(data []byte) (*Index, error) {
	if len(data) < 8 || string(data[:4]) != magic {
		return nil, errCorrupt
	}
	if v := binary.LittleEndian.Uint32(data[4:8]); v != formatVersion {
		return nil, errors.New("index was written by an incompatible eind version; run `eind index`")
	}
	r := &reader{buf: data, data: string(data), pos: 8}
	ix := &Index{BuiltAt: time.Unix(r.varint(), 0)}
	nroots := r.uvarint()
	for i := uint64(0); i < nroots && r.err == nil; i++ {
		ix.Roots = append(ix.Roots, r.str())
	}
	n := r.uvarint()
	if r.err != nil || n > uint64(len(data)) {
		return nil, errCorrupt
	}
	ix.Entries = make([]Entry, 0, n)
	for i := uint64(0); i < n; i++ {
		e := Entry{Name: r.str()}
		e.Parent = uint32(r.uvarint())
		e.Size = int64(r.uvarint())
		e.Modified = r.varint()
		e.Created = r.varint()
		e.IsDir = r.byte()&flagDir != 0
		if r.err != nil {
			return nil, r.err
		}
		ix.Entries = append(ix.Entries, e)
	}
	return ix, nil
}
