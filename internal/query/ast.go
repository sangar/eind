// Package query parses search strings into a small AST.
//
//	report ext:pdf;docx !draft size:>1mb dm:thisweek <foo|bar> path:"my dir"
//
// Whitespace is AND, "|" is OR, "!" negates, "<...>" groups, quotes protect
// spaces. A word can carry modifiers (case:, regex:, ww:, wfn:, path:,
// file:, folder:) and functions (ext:, size:, dm:, dc:, len:, depth:,
// parent:, infolder:).
package query

import "math"

type Node interface{ isNode() }

type And struct{ Kids []Node }
type Or struct{ Kids []Node }
type Not struct{ Kid Node }

type TextMode int

const (
	Substring TextMode = iota
	Wildcard
	Regex
	WholeWord
	WholeName
)

// Text matches against the file name, or the full path when Path is set.
type Text struct {
	Text          string
	Mode          TextMode
	Path          bool
	CaseSensitive bool
}

// Ext matches any of the given extensions (lowercase, no dot).
type Ext struct{ Exts []string }

// Range is inclusive on both ends.
type Range struct{ Lo, Hi int64 }

func (r Range) Contains(v int64) bool { return v >= r.Lo && v <= r.Hi }

var Unbounded = Range{Lo: math.MinInt64, Hi: math.MaxInt64}

type Size struct{ Range }
type Modified struct{ Range }
type Created struct{ Range }
type NameLen struct{ Range }
type Depth struct{ Range }

type IsDir struct{ Dir bool }

// Parent matches direct children of the directory; InFolder matches all descendants.
type Parent struct{ Path string }
type InFolder struct{ Path string }

func (And) isNode()      {}
func (Or) isNode()       {}
func (Not) isNode()      {}
func (Text) isNode()     {}
func (Ext) isNode()      {}
func (Size) isNode()     {}
func (Modified) isNode() {}
func (Created) isNode()  {}
func (NameLen) isNode()  {}
func (Depth) isNode()    {}
func (IsDir) isNode()    {}
func (Parent) isNode()   {}
func (InFolder) isNode() {}

// MatchAll is the empty query.
var MatchAll Node = And{}
