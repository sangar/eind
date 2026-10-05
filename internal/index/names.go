package index

import (
	"math/bits"
	"runtime"
	"slices"
	"strings"
	"sync"
	"unsafe"
)

// NameCandidates finds the base names containing needle by scanning all
// names as one buffer, which skips names that cannot match far faster than
// testing them one by one. It returns their ids in contains.
//
// With fold, needle must be lowercase ASCII and names match ignoring ASCII
// case, which their Unicode lowercasing agrees with except for a few names,
// such as one with a Kelvin sign that lowercases to "k". Those are returned
// in others, for the caller to test.
func (ix *Index) NameCandidates(needle string, fold bool) (contains, others []uint32) {
	buf := ix.base.names
	workers := runtime.GOMAXPROCS(0)
	parts := make([][]uint32, workers)
	var wg sync.WaitGroup
	start := 0
	for w := range workers {
		end := len(buf) * (w + 1) / workers
		if w < workers-1 && end > start {
			end += strings.IndexByte(unsafe.String(unsafe.SliceData(buf[end:]), len(buf)-end), 0) + 1
		}
		end = max(end, start)
		lo, hi := start, end
		start = end
		wg.Add(1)
		go func() {
			defer wg.Done()
			parts[w] = ix.scanNames(buf[lo:hi], lo, needle, fold)
		}()
	}
	wg.Wait()
	for _, p := range parts {
		contains = append(contains, p...)
	}
	if fold {
		for w, word := range ix.base.unicode {
			for ; word != 0; word &= word - 1 {
				others = append(others, uint32(w*64+bits.TrailingZeros64(word)))
			}
		}
	}
	return contains, others
}

func (ix *Index) scanNames(chunk []byte, offset int, needle string, fold bool) []uint32 {
	if len(chunk) == 0 {
		return nil
	}
	s := unsafe.String(unsafe.SliceData(chunk), len(chunk))
	var ids []uint32
	for pos := 0; pos < len(s); {
		var at int
		if fold {
			at = indexFold(s[pos:], needle)
		} else {
			at = strings.Index(s[pos:], needle)
		}
		if at < 0 {
			break
		}
		at += pos
		nameStart := strings.LastIndexByte(s[:at], 0) + 1
		id, _ := slices.BinarySearch(ix.base.nameOff, uint32(offset+nameStart))
		ids = append(ids, uint32(id))
		nameEnd := at + strings.IndexByte(s[at:], 0)
		pos = nameEnd + 1
	}
	return ids
}

// indexFold is strings.Index ignoring ASCII case, for a lowercase needle. It
// jumps between candidates for the needle's first byte in either case,
// looking for the uppercase one only up to the next lowercase one.
func indexFold(s, needle string) int {
	c := needle[0]
	upper := c
	if 'a' <= c && c <= 'z' {
		upper = c - 'a' + 'A'
	}
	for pos := 0; ; {
		i := len(s)
		if j := strings.IndexByte(s[pos:], c); j >= 0 {
			i = pos + j
		}
		if upper != c {
			if j := strings.IndexByte(s[pos:i], upper); j >= 0 {
				i = pos + j
			}
		}
		if i+len(needle) > len(s) {
			return -1
		}
		if equalFoldASCII(s[i:i+len(needle)], needle) {
			return i
		}
		pos = i + 1
	}
}

func equalFoldASCII(s, lower string) bool {
	for k := 0; k < len(s); k++ {
		c := s[k]
		if 'A' <= c && c <= 'Z' {
			c += 'a' - 'A'
		}
		if c != lower[k] {
			return false
		}
	}
	return true
}

// foldsBeyondASCII reports whether a name's Unicode lowercasing differs
// from lowercasing its ASCII letters alone.
func foldsBeyondASCII(name string) bool {
	b := []byte(name)
	for k, c := range b {
		if 'A' <= c && c <= 'Z' {
			b[k] = c + 'a' - 'A'
		}
	}
	return Lower(name) != string(b)
}
