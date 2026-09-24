package query

import "strings"

type tokenKind int

const (
	tokWord tokenKind = iota
	tokOr
	tokNot
	tokOpen
	tokClose
)

type token struct {
	kind tokenKind
	text string
}

// lex splits the query into words and operators. Inside a word, "<" and ">"
// are literal once a ":" has been seen, so size:>1mb and dm:<2020 work while
// <a|b> still groups. Quoted stretches are copied verbatim.
func lex(s string) []token {
	var toks []token
	i := 0
	for i < len(s) {
		c := s[i]
		switch {
		case isSpace(c):
			i++
		case c == '|':
			toks = append(toks, token{kind: tokOr})
			i++
		case c == '!':
			toks = append(toks, token{kind: tokNot})
			i++
		case c == '<':
			toks = append(toks, token{kind: tokOpen})
			i++
		case c == '>':
			toks = append(toks, token{kind: tokClose})
			i++
		default:
			word, next := lexWord(s, i)
			toks = append(toks, token{kind: tokWord, text: word})
			i = next
		}
	}
	return toks
}

func lexWord(s string, i int) (string, int) {
	var b strings.Builder
	sawColon := false
	for i < len(s) {
		c := s[i]
		if c == '"' {
			end := strings.IndexByte(s[i+1:], '"')
			if end < 0 {
				b.WriteString(s[i+1:])
				return b.String(), len(s)
			}
			b.WriteString(s[i+1 : i+1+end])
			i += end + 2
			continue
		}
		if isSpace(c) || c == '|' {
			break
		}
		if (c == '<' || c == '>') && !sawColon {
			break
		}
		if c == ':' {
			sawColon = true
		}
		b.WriteByte(c)
		i++
	}
	return b.String(), i
}

func isSpace(c byte) bool { return c == ' ' || c == '\t' || c == '\n' || c == '\r' }
