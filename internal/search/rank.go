package search

import (
	"cmp"
	"container/heap"
	"slices"
	"strings"

	"eind/internal/index"
	"eind/internal/query"
)

// Rank orders hits the way a launcher wants them: names that equal a search
// term come first, then names starting with it, then names containing it at
// a word boundary, then plain substring matches. Ties go to shallower paths,
// then to name order. Queries without plain name terms fall back to name order.
//
// Only the best keep hits are returned, in rank order; keep < 0 returns them
// all. Selecting the top few hundred out of a million is far cheaper than
// sorting a million, which is what keeps one-letter queries fast.
func Rank(ix *index.Index, hits []uint32, n query.Node, keep int) []uint32 {
	if keep < 0 || keep > len(hits) {
		keep = len(hits)
	}
	terms := plainTerms(n)
	if len(terms) == 0 {
		Sort(ix, hits, SortName, false)
		return hits[:keep]
	}
	rows := make([]ranked, len(hits))
	for k, h := range hits {
		rows[k] = ranked{hit: h, score: score(ix.Lower[h], terms), depth: depth(ix, h)}
	}
	better := func(a, b ranked) int {
		return cmp.Or(
			cmp.Compare(b.score, a.score),
			cmp.Compare(a.depth, b.depth),
			strings.Compare(ix.Lower[a.hit], ix.Lower[b.hit]),
			cmp.Compare(a.hit, b.hit),
		)
	}
	if keep*4 > len(rows) {
		slices.SortFunc(rows, better)
		rows = rows[:keep]
	} else {
		rows = topK(rows, keep, better)
	}
	out := make([]uint32, len(rows))
	for k := range rows {
		out[k] = rows[k].hit
	}
	return out
}

type ranked struct {
	hit   uint32
	score int
	depth int
}

// topK keeps the k best rows using a heap whose top is the worst kept row.
func topK(rows []ranked, k int, better func(a, b ranked) int) []ranked {
	if k == 0 {
		return nil
	}
	h := &worstFirst{rows: make([]ranked, 0, k+1), better: better}
	for _, r := range rows {
		if h.Len() < k {
			heap.Push(h, r)
		} else if better(r, h.rows[0]) < 0 {
			h.rows[0] = r
			heap.Fix(h, 0)
		}
	}
	slices.SortFunc(h.rows, better)
	return h.rows
}

type worstFirst struct {
	rows   []ranked
	better func(a, b ranked) int
}

func (h *worstFirst) Len() int           { return len(h.rows) }
func (h *worstFirst) Less(i, j int) bool { return h.better(h.rows[i], h.rows[j]) > 0 }
func (h *worstFirst) Swap(i, j int)      { h.rows[i], h.rows[j] = h.rows[j], h.rows[i] }
func (h *worstFirst) Push(x any)         { h.rows = append(h.rows, x.(ranked)) }
func (h *worstFirst) Pop() any {
	last := h.rows[len(h.rows)-1]
	h.rows = h.rows[:len(h.rows)-1]
	return last
}

const (
	scoreExact     = 4
	scorePrefix    = 3
	scoreWordStart = 2
	scoreContains  = 1
)

func score(lowerName string, terms []string) int {
	stem := lowerName
	if dot := strings.LastIndexByte(stem, '.'); dot > 0 {
		stem = stem[:dot]
	}
	total := 0
	for _, t := range terms {
		switch {
		case lowerName == t || stem == t:
			total += scoreExact
		case strings.HasPrefix(lowerName, t):
			total += scorePrefix
		case startsWordIn(lowerName, t):
			total += scoreWordStart
		case strings.Contains(lowerName, t):
			total += scoreContains
		}
	}
	return total
}

func startsWordIn(hay, needle string) bool {
	for off := 0; ; {
		i := strings.Index(hay[off:], needle)
		if i < 0 {
			return false
		}
		if boundaryBefore(hay, off+i) {
			return true
		}
		off += i + 1
	}
}

func depth(ix *index.Index, i uint32) int {
	d := 0
	for p := ix.Entries[i].Parent; p != index.NoParent; p = ix.Entries[p].Parent {
		d++
	}
	return d
}

// plainTerms collects the lowercased name terms a user typed, ignoring
// negated, path, wildcard and regex terms, which carry no ranking signal.
func plainTerms(n query.Node) []string {
	var terms []string
	var walk func(n query.Node)
	walk = func(n query.Node) {
		switch n := n.(type) {
		case query.And:
			for _, k := range n.Kids {
				walk(k)
			}
		case query.Or:
			for _, k := range n.Kids {
				walk(k)
			}
		case query.Text:
			if !n.Path && (n.Mode == query.Substring || n.Mode == query.WholeWord || n.Mode == query.WholeName) {
				terms = append(terms, strings.ToLower(n.Text))
			}
		}
	}
	walk(n)
	return terms
}
