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
	path   bool
	cased  bool
}

func (m textM) match(c *ctx) bool {
	var hay string
	switch {
	case m.re != nil && m.path:
		hay = c.Path()
	case m.re != nil:
		hay = c.ix.Entries[c.i].Name
	case m.path && m.cased:
		hay = c.Path()
	case m.path:
		hay = c.PathLower()
	case m.cased:
		hay = c.ix.Entries[c.i].Name
	default:
		hay = c.ix.Lower[c.i]
	}
	if m.re != nil {
		return m.re.MatchString(hay)
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

type extM struct{ exts []string }

func (m extM) match(c *ctx) bool {
	ext := c.ix.Ext(c.i)
	for _, e := range m.exts {
		if e == ext {
			return true
		}
	}
	return false
}

type fieldM struct {
	r     query.Range
	value func(c *ctx) int64
}

func (m fieldM) match(c *ctx) bool { return m.r.Contains(m.value(c)) }

type isDirM struct{ dir bool }

func (m isDirM) match(c *ctx) bool { return c.ix.Entries[c.i].IsDir == m.dir }

type parentM struct{ dir uint32 }

func (m parentM) match(c *ctx) bool { return c.ix.Entries[c.i].Parent == m.dir }

type inFolderM struct{ dir uint32 }

func (m inFolderM) match(c *ctx) bool {
	for p := c.ix.Entries[c.i].Parent; p != index.NoParent; p = c.ix.Entries[p].Parent {
		if p == m.dir {
			return true
		}
	}
	return false
}

func compile(ix *index.Index, n query.Node) (matcher, error) {
	switch n := n.(type) {
	case query.And:
		if len(n.Kids) == 0 {
			return allM{}, nil
		}
		kids, err := compileAll(ix, n.Kids)
		return andM{kids}, err
	case query.Or:
		kids, err := compileAll(ix, n.Kids)
		return orM{kids}, err
	case query.Not:
		kid, err := compile(ix, n.Kid)
		return notM{kid}, err
	case query.Text:
		return compileText(n)
	case query.Ext:
		return extM{n.Exts}, nil
	case query.Size:
		return fieldM{n.Range, func(c *ctx) int64 { return c.ix.Entries[c.i].Size }}, nil
	case query.Modified:
		return fieldM{n.Range, func(c *ctx) int64 { return c.ix.Entries[c.i].Modified }}, nil
	case query.Created:
		return fieldM{n.Range, func(c *ctx) int64 { return c.ix.Entries[c.i].Created }}, nil
	case query.NameLen:
		return fieldM{n.Range, func(c *ctx) int64 { return int64(utf8.RuneCountInString(c.ix.Entries[c.i].Name)) }}, nil
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

func compileAll(ix *index.Index, nodes []query.Node) ([]matcher, error) {
	kids := make([]matcher, 0, len(nodes))
	for _, k := range nodes {
		m, err := compile(ix, k)
		if err != nil {
			return nil, err
		}
		kids = append(kids, m)
	}
	return kids, nil
}

func compileText(t query.Text) (matcher, error) {
	m := textM{mode: t.Mode, path: t.Path, cased: t.CaseSensitive}
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
			return nil, fmt.Errorf("invalid regex %q: %w", t.Text, err)
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
	for i := range ix.Entries {
		e := &ix.Entries[i]
		if e.Parent != index.NoParent || !e.IsDir {
			continue
		}
		root := strings.TrimSuffix(strings.ToLower(e.Name), sep)
		if target == root || (root == "" && target == "") {
			return uint32(i), true
		}
		if !strings.HasPrefix(target, root+sep) {
			continue
		}
		cur := uint32(i)
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
	for i := range ix.Entries {
		e := &ix.Entries[i]
		if e.Parent == parent && e.IsDir && ix.Lower[i] == lowerName {
			return uint32(i), true
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
	m, err := compile(ix, n)
	if err != nil {
		return nil, err
	}
	total := len(ix.Entries)
	workers := runtime.GOMAXPROCS(0)
	chunk := (total + workers - 1) / workers
	if chunk < 4096 {
		chunk = 4096
	}
	var parts [][]uint32
	var wg sync.WaitGroup
	for lo := 0; lo < total; lo += chunk {
		hi := min(lo+chunk, total)
		parts = append(parts, nil)
		slot := len(parts) - 1
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
