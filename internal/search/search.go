// Package search evaluates a parsed query against an index.
package search

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"sync"
	"unicode"
	"unicode/utf8"

	"eind/internal/index"
	"eind/internal/query"
)

// ctx carries one entry through the matcher tree, building its full path
// only if some matcher asks for it.
const cancelCheckInterval = 8192

type ctx struct {
	ix        *index.Index
	i         uint32
	path      string
	pathLower string
	havePath  bool
	haveLower bool
}

func (c *ctx) reset(i uint32) {
	c.i = i
	c.havePath = false
	c.haveLower = false
}

func (c *ctx) Path() string {
	if !c.havePath {
		c.path = c.ix.Path(c.i)
		c.havePath = true
	}
	return c.path
}

func (c *ctx) PathLower() string {
	if !c.haveLower {
		c.pathLower = strings.ToLower(c.Path())
		c.haveLower = true
	}
	return c.pathLower
}

type matcher interface{ match(c *ctx) bool }

type allM struct{}
type noneM struct{}
type andM struct{ kids []matcher }
type orM struct{ kids []matcher }
type notM struct{ kid matcher }

func (allM) match(*ctx) bool  { return true }
func (noneM) match(*ctx) bool { return false }

func (m andM) match(c *ctx) bool {
	for _, k := range m.kids {
		if !k.match(c) {
			return false
		}
	}
	return true
}

func (m orM) match(c *ctx) bool {
	for _, k := range m.kids {
		if k.match(c) {
			return true
		}
	}
	return false
}

func (m notM) match(c *ctx) bool { return !m.kid.match(c) }

type textM struct {
	needle string
	re     *regexp.Regexp
	mode   query.TextMode
	glob   globKind
	path   bool
	cased  bool
}

// globKind is a wildcard pattern simple enough to match without a regexp:
// "*x", "x*" and "*x*" are by far the most typed, and a leading "*" alone
// would otherwise run a regexp over every name in the index.
type globKind int

const (
	notSimpleGlob globKind = iota
	globSuffix
	globPrefix
	globContains
)

func simpleGlob(pattern string) (globKind, string) {
	if strings.ContainsRune(pattern, '?') {
		return notSimpleGlob, ""
	}
	inner := strings.Trim(pattern, "*")
	if strings.ContainsRune(inner, '*') || inner == pattern {
		return notSimpleGlob, ""
	}
	switch {
	case strings.HasPrefix(pattern, "*") && strings.HasSuffix(pattern, "*"):
		return globContains, inner
	case strings.HasPrefix(pattern, "*"):
		return globSuffix, inner
	default:
		return globPrefix, inner
	}
}

// match is used for path terms only; name terms are compiled to a nameM.
func (m textM) match(c *ctx) bool {
	if m.re != nil || m.cased {
		return m.matchHay(c.Path())
	}
	return m.matchHay(c.PathLower())
}

func (m textM) matchName(name, lower string) bool {
	if m.re != nil || m.cased {
		return m.matchHay(name)
	}
	return m.matchHay(lower)
}

func (m textM) matchHay(hay string) bool {
	if m.re != nil {
		return m.re.MatchString(hay)
	}
	switch m.glob {
	case globSuffix:
		return strings.HasSuffix(hay, m.needle)
	case globPrefix:
		return strings.HasPrefix(hay, m.needle)
	case globContains:
		return strings.Contains(hay, m.needle)
	}
	switch m.mode {
	case query.WholeName:
		return hay == m.needle
	case query.WholeWord:
		return containsWord(hay, m.needle)
	default:
		return strings.Contains(hay, m.needle)
	}
}

func containsWord(hay, needle string) bool {
	if needle == "" {
		return true
	}
	for off := 0; ; {
		i := strings.Index(hay[off:], needle)
		if i < 0 {
			return false
		}
		start := off + i
		end := start + len(needle)
		if boundaryBefore(hay, start) && boundaryAfter(hay, end) {
			return true
		}
		off = start + 1
	}
}

func boundaryBefore(s string, i int) bool {
	if i == 0 {
		return true
	}
	r, _ := utf8.DecodeLastRuneInString(s[:i])
	return !isWordRune(r)
}

func boundaryAfter(s string, i int) bool {
	if i >= len(s) {
		return true
	}
	r, _ := utf8.DecodeRuneInString(s[i:])
	return !isWordRune(r)
}

// Underscores separate words so that ww:main finds main_test.go.
func isWordRune(r rune) bool { return unicode.IsLetter(r) || unicode.IsDigit(r) }

// nameM is a test that depends on nothing but an entry's name. It runs once
// per distinct name before the scan, so that the scan looks up one bit per
// entry instead of matching a million names, most of which repeat.
type nameM struct {
	test func(name, lower string) bool
	bits []uint64 // over the index's distinct names, filled by prepare
	scan nameScan
}

// nameScan narrows the names to test to those containing a needle, which the
// index finds by scanning all names at once.
type nameScan struct {
	needle string // "" when the test has no such needle
	fold   bool   // the needle is lowercase ASCII, matched ignoring ASCII case
	exact  bool   // containing the needle passes the test, no need to run it
}

func (m *nameM) match(c *ctx) bool {
	if id, ok := c.ix.NameID(c.i); ok {
		return m.bits[id/64]&(1<<(id%64)) != 0
	}
	name := c.ix.Name(c.i)
	return m.test(name, index.Lower(name))
}

// prepare tests every distinct name, or only the candidates of its scan.
func (m *nameM) prepare(ix *index.Index) {
	n := ix.DistinctNames()
	m.bits = make([]uint64, (n+63)/64)
	if m.scan.needle != "" {
		m.prepareScan(ix)
		return
	}
	words := len(m.bits)
	workers := runtime.GOMAXPROCS(0)
	var wg sync.WaitGroup
	for w := range workers {
		lo, hi := words*w/workers, words*(w+1)/workers
		wg.Add(1)
		go func() {
			defer wg.Done()
			var scratch []byte
			for id := lo * 64; id < min(hi*64, n); id++ {
				name := ix.DistinctName(uint32(id))
				if m.test(name, index.LowerInto(&scratch, name)) {
					m.bits[id/64] |= 1 << (id % 64)
				}
			}
		}()
	}
	wg.Wait()
}

func (m *nameM) prepareScan(ix *index.Index) {
	contains, others := ix.NameCandidates(m.scan.needle, m.scan.fold)
	if !m.scan.exact {
		others = append(others, contains...)
		contains = nil
	}
	for _, id := range contains {
		m.bits[id/64] |= 1 << (id % 64)
	}
	for _, id := range others {
		name := ix.DistinctName(id)
		if m.test(name, index.Lower(name)) {
			m.bits[id/64] |= 1 << (id % 64)
		}
	}
}

// scanFor describes the needle a text test requires, if any.
func (m textM) scanFor() nameScan {
	if m.re != nil || m.needle == "" || !m.cased && !isASCII(m.needle) {
		return nameScan{}
	}
	plain := m.glob == globContains || m.glob == notSimpleGlob && m.mode == query.Substring
	return nameScan{needle: m.needle, fold: !m.cased, exact: plain}
}

func isASCII(s string) bool {
	for k := 0; k < len(s); k++ {
		if s[k] >= utf8.RuneSelf {
			return false
		}
	}
	return true
}

func extTest(exts []string) func(name, lower string) bool {
	return func(_, lower string) bool {
		ext := index.Ext(lower)
		for _, e := range exts {
			if e == ext {
				return true
			}
		}
		return false
	}
}

type fieldM struct {
	r     query.Range
	value func(c *ctx) int64
}

func (m fieldM) match(c *ctx) bool { return m.r.Contains(m.value(c)) }

type isDirM struct{ dir bool }

func (m isDirM) match(c *ctx) bool { return c.ix.IsDir(c.i) == m.dir }

type parentM struct{ dir uint32 }

func (m parentM) match(c *ctx) bool { return c.ix.Parent(c.i) == m.dir }

type inFolderM struct{ dir uint32 }

func (m inFolderM) match(c *ctx) bool {
	for p := c.ix.Parent(c.i); p != index.NoParent; p = c.ix.Parent(p) {
		if p == m.dir {
			return true
		}
	}
	return false
}

// compiler collects the name tests of a query so they can be prepared
// before the scan.
type compiler struct {
	ix    *index.Index
	names []*nameM
}

func (cp *compiler) nameTest(test func(name, lower string) bool, scan nameScan) matcher {
	m := &nameM{test: test, scan: scan}
	cp.names = append(cp.names, m)
	return m
}

func (cp *compiler) compile(n query.Node) (matcher, error) {
	ix := cp.ix
	switch n := n.(type) {
	case query.And:
		if len(n.Kids) == 0 {
			return allM{}, nil
		}
		kids, err := cp.compileAll(n.Kids)
		return andM{kids}, err
	case query.Or:
		kids, err := cp.compileAll(n.Kids)
		return orM{kids}, err
	case query.Not:
		kid, err := cp.compile(n.Kid)
		return notM{kid}, err
	case query.Text:
		m, err := compileText(n)
		if err != nil || n.Path {
			return m, err
		}
		return cp.nameTest(m.matchName, m.scanFor()), nil
	case query.Ext:
		var scan nameScan
		if len(n.Exts) == 1 && n.Exts[0] != "" && isASCII(n.Exts[0]) {
			scan = nameScan{needle: "." + n.Exts[0], fold: true}
		}
		return cp.nameTest(extTest(n.Exts), scan), nil
	case query.Size:
		return fieldM{n.Range, func(c *ctx) int64 { return c.ix.Size(c.i) }}, nil
	case query.Modified:
		return fieldM{n.Range, func(c *ctx) int64 { return c.ix.Modified(c.i) }}, nil
	case query.Created:
		return fieldM{n.Range, func(c *ctx) int64 { return c.ix.Created(c.i) }}, nil
	case query.NameLen:
		r := n.Range
		return cp.nameTest(func(name, _ string) bool { return r.Contains(int64(utf8.RuneCountInString(name))) }, nameScan{}), nil
	case query.Depth:
		return fieldM{n.Range, func(c *ctx) int64 { return int64(depthOf(c.Path())) }}, nil
	case query.IsDir:
		return isDirM{n.Dir}, nil
	case query.Parent:
		dir, ok := ResolveDir(ix, n.Path)
		if !ok {
			return noneM{}, nil
		}
		return parentM{dir}, nil
	case query.InFolder:
		dir, ok := ResolveDir(ix, n.Path)
		if !ok {
			return noneM{}, nil
		}
		return inFolderM{dir}, nil
	}
	return nil, fmt.Errorf("unsupported query node %T", n)
}

func (cp *compiler) compileAll(nodes []query.Node) ([]matcher, error) {
	kids := make([]matcher, 0, len(nodes))
	for _, k := range nodes {
		m, err := cp.compile(k)
		if err != nil {
			return nil, err
		}
		kids = append(kids, m)
	}
	return kids, nil
}

func compileText(t query.Text) (textM, error) {
	m := textM{mode: t.Mode, path: t.Path, cased: t.CaseSensitive}
	if t.Mode == query.Wildcard {
		if kind, needle := simpleGlob(t.Text); kind != notSimpleGlob {
			m.glob, m.needle = kind, needle
			if !t.CaseSensitive {
				m.needle = strings.ToLower(needle)
			}
			return m, nil
		}
	}
	switch t.Mode {
	case query.Regex, query.Wildcard:
		pattern := t.Text
		if t.Mode == query.Wildcard {
			pattern = wildcardToRegexp(t.Text)
		}
		if !t.CaseSensitive {
			pattern = "(?i)" + pattern
		}
		re, err := regexp.Compile(pattern)
		if err != nil {
			return textM{}, fmt.Errorf("invalid regex %q: %w", t.Text, err)
		}
		m.re = re
	default:
		m.needle = t.Text
		if !t.CaseSensitive {
			m.needle = strings.ToLower(t.Text)
		}
	}
	return m, nil
}

// wildcardToRegexp anchors the pattern: a wildcard term must
// match the whole name.
func wildcardToRegexp(glob string) string {
	var b strings.Builder
	b.WriteByte('^')
	for _, r := range glob {
		switch r {
		case '*':
			b.WriteString(".*")
		case '?':
			b.WriteByte('.')
		default:
			b.WriteString(regexp.QuoteMeta(string(r)))
		}
	}
	b.WriteByte('$')
	return b.String()
}

// depthOf counts path components below the filesystem root.
func depthOf(path string) int {
	sep := string(os.PathSeparator)
	path = strings.TrimSuffix(path, sep)
	if path == "" {
		return 0
	}
	n := strings.Count(path, sep)
	if filepath.VolumeName(path) != "" {
		n--
	}
	return n
}

// ResolveDir finds the index entry for a directory path, case-insensitively.
func ResolveDir(ix *index.Index, path string) (uint32, bool) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return 0, false
	}
	target := strings.ToLower(filepath.Clean(abs))
	sep := string(os.PathSeparator)
	for i := range uint32(ix.Count()) {
		if ix.Parent(i) != index.NoParent || !ix.IsDir(i) || !ix.Live(i) {
			continue
		}
		root := strings.TrimSuffix(index.Lower(ix.Name(i)), sep)
		if target == root || (root == "" && target == "") {
			return i, true
		}
		if !strings.HasPrefix(target, root+sep) {
			continue
		}
		cur := i
		for _, comp := range strings.Split(target[len(root)+1:], sep) {
			next, ok := findChildDir(ix, cur, comp)
			if !ok {
				return 0, false
			}
			cur = next
		}
		return cur, true
	}
	return 0, false
}

func findChildDir(ix *index.Index, parent uint32, lowerName string) (uint32, bool) {
	for i := range uint32(ix.Count()) {
		if ix.Parent(i) == parent && ix.IsDir(i) && ix.Live(i) && index.Lower(ix.Name(i)) == lowerName {
			return i, true
		}
	}
	return 0, false
}

// Run returns the indices of all live entries matching the query, in index order.
func Run(ix *index.Index, n query.Node) ([]uint32, error) {
	return RunContext(context.Background(), ix, n)
}

// RunContext is Run with cancellation; it returns ctx.Err() once cancelled.
func RunContext(cancel context.Context, ix *index.Index, n query.Node) ([]uint32, error) {
	cp := &compiler{ix: ix}
	m, err := cp.compile(n)
	if err != nil {
		return nil, err
	}
	for _, nm := range cp.names {
		nm.prepare(ix)
	}
	total := ix.Count()
	workers := runtime.GOMAXPROCS(0)
	chunk := (total + workers - 1) / workers
	if chunk < 4096 {
		chunk = 4096
	}
	parts := make([][]uint32, (total+chunk-1)/chunk)
	var wg sync.WaitGroup
	for lo := 0; lo < total; lo += chunk {
		hi := min(lo+chunk, total)
		slot := lo / chunk
		wg.Add(1)
		go func(lo, hi, slot int) {
			defer wg.Done()
			c := &ctx{ix: ix}
			var hits []uint32
			for i := lo; i < hi; i++ {
				if i%cancelCheckInterval == 0 && cancel.Err() != nil {
					return
				}
				if !ix.Live(uint32(i)) {
					continue
				}
				c.reset(uint32(i))
				if m.match(c) {
					hits = append(hits, uint32(i))
				}
			}
			parts[slot] = hits
		}(lo, hi, slot)
	}
	wg.Wait()
	if err := cancel.Err(); err != nil {
		return nil, err
	}
	n2 := 0
	for _, p := range parts {
		n2 += len(p)
	}
	hits := make([]uint32, 0, n2)
	for _, p := range parts {
		hits = append(hits, p...)
	}
	return hits, nil
}
