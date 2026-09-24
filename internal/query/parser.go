package query

import (
	"fmt"
	"strings"
	"time"
)

// Defaults come from command-line switches and apply to plain words.
type Defaults struct {
	Regex         bool
	CaseSensitive bool
	WholeWord     bool
	MatchPath     bool
	// Now anchors relative dates such as dm:today; zero means time.Now().
	Now time.Time
}

func Parse(s string, d Defaults) (Node, error) {
	if d.Now.IsZero() {
		d.Now = time.Now()
	}
	p := &parser{toks: lex(s), d: d}
	node := p.or()
	if p.err != nil {
		return nil, p.err
	}
	return node, nil
}

type parser struct {
	toks []token
	pos  int
	d    Defaults
	err  error
}

func (p *parser) peek() (token, bool) {
	if p.pos >= len(p.toks) {
		return token{}, false
	}
	return p.toks[p.pos], true
}

func (p *parser) or() Node {
	kids := []Node{p.and()}
	for {
		t, ok := p.peek()
		if !ok || t.kind != tokOr {
			break
		}
		p.pos++
		kids = append(kids, p.and())
	}
	if len(kids) == 1 {
		return kids[0]
	}
	return Or{Kids: kids}
}

func (p *parser) and() Node {
	var kids []Node
	for {
		t, ok := p.peek()
		if !ok || t.kind == tokOr || t.kind == tokClose {
			break
		}
		kids = append(kids, p.unary())
	}
	if len(kids) == 1 {
		return kids[0]
	}
	return And{Kids: kids}
}

func (p *parser) unary() Node {
	t := p.toks[p.pos]
	p.pos++
	switch t.kind {
	case tokNot:
		if _, ok := p.peek(); !ok {
			return Not{Kid: MatchAll}
		}
		return Not{Kid: p.unary()}
	case tokOpen:
		node := p.or()
		if t, ok := p.peek(); ok && t.kind == tokClose {
			p.pos++
		}
		return node
	case tokWord:
		return p.word(t.text)
	}
	return MatchAll
}

// word peels modifier and function prefixes off a word such as
// "folder:case:regex:^foo" and builds the matching node.
func (p *parser) word(w string) Node {
	t := Text{Path: p.d.MatchPath, CaseSensitive: p.d.CaseSensitive}
	var typeFilter Node
	modeSet := false
	noWildcards := false
	rest := w
	wrap := func(n Node) Node {
		if typeFilter != nil {
			return And{Kids: []Node{typeFilter, n}}
		}
		return n
	}
	fail := func(key string, err error) Node {
		if p.err == nil {
			p.err = fmt.Errorf("%s: %w", key, err)
		}
		return MatchAll
	}
	for {
		colon := strings.IndexByte(rest, ':')
		if colon < 0 {
			break
		}
		key, arg := strings.ToLower(rest[:colon]), rest[colon+1:]
		switch key {
		case "case":
			t.CaseSensitive = true
		case "nocase":
			t.CaseSensitive = false
		case "regex":
			t.Mode, modeSet = Regex, true
		case "wfn", "wholefilename", "exact":
			t.Mode, modeSet = WholeName, true
		case "ww", "wholeword":
			t.Mode, modeSet = WholeWord, true
		case "wildcards":
			t.Mode, modeSet = Wildcard, true
		case "nowildcards":
			noWildcards = true
		case "path":
			t.Path = true
		case "nopath":
			t.Path = false
		case "file", "files":
			typeFilter = IsDir{Dir: false}
		case "folder", "folders", "dir", "dirs":
			typeFilter = IsDir{Dir: true}
		case "ext":
			return wrap(Ext{Exts: splitExts(arg)})
		case "size":
			r, err := parseRange(arg, parseSizeValue)
			if err != nil {
				return fail(key, err)
			}
			return wrap(Size{r})
		case "dm", "datemodified":
			r, err := parseRange(arg, dateParser(p.d.Now))
			if err != nil {
				return fail(key, err)
			}
			return wrap(Modified{r})
		case "dc", "datecreated":
			r, err := parseRange(arg, dateParser(p.d.Now))
			if err != nil {
				return fail(key, err)
			}
			return wrap(Created{r})
		case "len":
			r, err := parseRange(arg, parseIntValue)
			if err != nil {
				return fail(key, err)
			}
			return wrap(NameLen{r})
		case "depth", "parents":
			r, err := parseRange(arg, parseIntValue)
			if err != nil {
				return fail(key, err)
			}
			return wrap(Depth{r})
		case "parent":
			return wrap(Parent{Path: arg})
		case "infolder":
			return wrap(InFolder{Path: arg})
		default:
			return wrap(p.text(t, rest, modeSet, noWildcards))
		}
		rest = arg
	}
	if rest == "" {
		if typeFilter != nil {
			return typeFilter
		}
		return MatchAll
	}
	return wrap(p.text(t, rest, modeSet, noWildcards))
}

func (p *parser) text(t Text, s string, modeSet, noWildcards bool) Node {
	t.Text = s
	if !modeSet {
		switch {
		case p.d.Regex:
			t.Mode = Regex
		case !noWildcards && strings.ContainsAny(s, "*?"):
			t.Mode = Wildcard
		case p.d.WholeWord:
			t.Mode = WholeWord
		}
	}
	return t
}

func splitExts(arg string) []string {
	var exts []string
	for _, e := range strings.FieldsFunc(arg, func(r rune) bool { return r == ';' || r == ',' }) {
		e = strings.ToLower(strings.TrimPrefix(strings.TrimSpace(e), "."))
		if e != "" {
			exts = append(exts, e)
		}
	}
	return exts
}
