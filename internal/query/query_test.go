package query

import (
	"math"
	"reflect"
	"testing"
	"time"
)

func TestParseBuildsQuerySyntax(t *testing.T) {
	sub := func(s string) Text { return Text{Text: s} }
	cases := []struct {
		query string
		want  Node
	}{
		{"", MatchAll},
		{"report", sub("report")},
		{"foo bar", And{Kids: []Node{sub("foo"), sub("bar")}}},
		{"foo|bar", Or{Kids: []Node{sub("foo"), sub("bar")}}},
		{"a b | c", Or{Kids: []Node{And{Kids: []Node{sub("a"), sub("b")}}, sub("c")}}},
		{"!draft", Not{Kid: sub("draft")}},
		{"<a|b> c", And{Kids: []Node{Or{Kids: []Node{sub("a"), sub("b")}}, sub("c")}}},
		{`path:"my dir"`, Text{Text: "my dir", Path: true}},
		{`"two words"`, sub("two words")},
		{"*.txt", Text{Text: "*.txt", Mode: Wildcard}},
		{"case:Readme", Text{Text: "Readme", CaseSensitive: true}},
		{`regex:^img_\d+`, Text{Text: `^img_\d+`, Mode: Regex}},
		{"ww:log", Text{Text: "log", Mode: WholeWord}},
		{"wfn:Makefile", Text{Text: "Makefile", Mode: WholeName}},
		{"folder:case:src", And{Kids: []Node{IsDir{Dir: true}, Text{Text: "src", CaseSensitive: true}}}},
		{"file:", IsDir{Dir: false}},
		{"ext:pdf;.DOCX", Ext{Exts: []string{"pdf", "docx"}}},
		{"size:>1kb", Size{Range{Lo: 1025, Hi: math.MaxInt64}}},
		{"size:1kb..2kb", Size{Range{Lo: 1024, Hi: 2048}}},
		{"size:<1kb", Size{Range{Lo: math.MinInt64, Hi: 1023}}},
		{"len:>40", NameLen{Range{Lo: 41, Hi: math.MaxInt64}}},
		{"depth:3", Depth{Range{Lo: 3, Hi: 3}}},
		{"parent:/tmp", Parent{Path: "/tmp"}},
		{"infolder:/tmp", InFolder{Path: "/tmp"}},
		{"c:notafunction", sub("c:notafunction")},
	}
	for _, c := range cases {
		got, err := Parse(c.query, Defaults{})
		if err != nil {
			t.Fatalf("%q: %v", c.query, err)
		}
		if !reflect.DeepEqual(got, c.want) {
			t.Errorf("%q\n got %#v\nwant %#v", c.query, got, c.want)
		}
	}
}

func TestDefaultsApplyToPlainWordsOnly(t *testing.T) {
	got, err := Parse("foo ext:txt", Defaults{Regex: true, CaseSensitive: true, MatchPath: true})
	if err != nil {
		t.Fatal(err)
	}
	want := And{Kids: []Node{Text{Text: "foo", Mode: Regex, CaseSensitive: true, Path: true}, Ext{Exts: []string{"txt"}}}}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("got %#v, want %#v", got, want)
	}
}

func TestParseReportsBadValues(t *testing.T) {
	for _, q := range []string{"size:big", "dm:someday", "len:x"} {
		if _, err := Parse(q, Defaults{}); err == nil {
			t.Errorf("%q: expected an error", q)
		}
	}
}

func TestSizeValues(t *testing.T) {
	cases := map[string]Range{
		"0":        {0, 0},
		"1.5kb":    {1536, 1536},
		"2 MB":     {2 * mb, 2 * mb},
		"3g":       {3 * gb, 3 * gb},
		"empty":    {0, 0},
		"gigantic": {128*mb + 1, math.MaxInt64},
	}
	for in, want := range cases {
		got, err := parseSizeValue(in)
		if err != nil || got != want {
			t.Errorf("%q: got %v, %v; want %v", in, got, err, want)
		}
	}
}

func TestDateValues(t *testing.T) {
	loc := time.FixedZone("test", 0)
	now := time.Date(2024, 3, 13, 15, 0, 0, 0, loc) // a Wednesday
	day := func(y int, m time.Month, d int) int64 { return time.Date(y, m, d, 0, 0, 0, 0, loc).Unix() }
	cases := map[string]Range{
		"today":      {day(2024, 3, 13), day(2024, 3, 14) - 1},
		"yesterday":  {day(2024, 3, 12), day(2024, 3, 13) - 1},
		"thisweek":   {day(2024, 3, 11), day(2024, 3, 18) - 1},
		"lastweek":   {day(2024, 3, 4), day(2024, 3, 11) - 1},
		"thismonth":  {day(2024, 3, 1), day(2024, 4, 1) - 1},
		"lastmonth":  {day(2024, 2, 1), day(2024, 3, 1) - 1},
		"thisyear":   {day(2024, 1, 1), day(2025, 1, 1) - 1},
		"lastyear":   {day(2023, 1, 1), day(2024, 1, 1) - 1},
		"last7days":  {now.AddDate(0, 0, -7).Unix(), now.Unix()},
		"2023":       {day(2023, 1, 1), day(2024, 1, 1) - 1},
		"2023-11":    {day(2023, 11, 1), day(2023, 12, 1) - 1},
		"2023/11/05": {day(2023, 11, 5), day(2023, 11, 6) - 1},
	}
	for in, want := range cases {
		got, err := parseDateValue(in, now)
		if err != nil || got != want {
			t.Errorf("%q: got %v, %v; want %v", in, got, err, want)
		}
	}
	after, err := parseRange(">2023", dateParser(now))
	if err != nil || after != (Range{Lo: day(2024, 1, 1), Hi: math.MaxInt64}) {
		t.Errorf(">2023: got %v, %v", after, err)
	}
}
