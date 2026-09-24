package query

import (
	"errors"
	"fmt"
	"math"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// A valueParser turns one value such as "1mb" or "2024-03" into the inclusive
// range it covers; comparison operators are then applied to that range.
type valueParser func(s string) (Range, error)

func parseRange(s string, parse valueParser) (Range, error) {
	s = strings.TrimSpace(s)
	switch {
	case strings.HasPrefix(s, ">="):
		v, err := parse(s[2:])
		return Range{Lo: v.Lo, Hi: math.MaxInt64}, err
	case strings.HasPrefix(s, "<="):
		v, err := parse(s[2:])
		return Range{Lo: math.MinInt64, Hi: v.Hi}, err
	case strings.HasPrefix(s, ">"):
		v, err := parse(s[1:])
		return Range{Lo: v.Hi + 1, Hi: math.MaxInt64}, err
	case strings.HasPrefix(s, "<"):
		v, err := parse(s[1:])
		return Range{Lo: math.MinInt64, Hi: v.Lo - 1}, err
	case strings.HasPrefix(s, "="):
		return parse(s[1:])
	}
	if i := strings.Index(s, ".."); i >= 0 {
		r := Unbounded
		if lo := s[:i]; lo != "" {
			v, err := parse(lo)
			if err != nil {
				return r, err
			}
			r.Lo = v.Lo
		}
		if hi := s[i+2:]; hi != "" {
			v, err := parse(hi)
			if err != nil {
				return r, err
			}
			r.Hi = v.Hi
		}
		return r, nil
	}
	return parse(s)
}

func parseIntValue(s string) (Range, error) {
	n, err := strconv.ParseInt(strings.TrimSpace(s), 10, 64)
	if err != nil {
		return Range{}, fmt.Errorf("expected a number, got %q", s)
	}
	return Range{Lo: n, Hi: n}, nil
}

const (
	kb = int64(1) << 10
	mb = kb << 10
	gb = mb << 10
	tb = gb << 10
	pb = tb << 10
)

var namedSizes = map[string]Range{
	"empty":    {0, 0},
	"tiny":     {0, 10 * kb},
	"small":    {10*kb + 1, 100 * kb},
	"medium":   {100*kb + 1, mb},
	"large":    {mb + 1, 16 * mb},
	"huge":     {16*mb + 1, 128 * mb},
	"gigantic": {128*mb + 1, math.MaxInt64},
}

var sizeUnits = map[string]int64{
	"": 1, "b": 1,
	"k": kb, "kb": kb, "kib": kb,
	"m": mb, "mb": mb, "mib": mb,
	"g": gb, "gb": gb, "gib": gb,
	"t": tb, "tb": tb, "tib": tb,
	"p": pb, "pb": pb, "pib": pb,
}

var sizePattern = regexp.MustCompile(`^(\d+(?:\.\d+)?)\s*([a-z]*)$`)

func parseSizeValue(s string) (Range, error) {
	s = strings.ToLower(strings.TrimSpace(s))
	if r, ok := namedSizes[s]; ok {
		return r, nil
	}
	m := sizePattern.FindStringSubmatch(s)
	if m == nil {
		return Range{}, fmt.Errorf("expected a size such as 10mb, got %q", s)
	}
	unit, ok := sizeUnits[m[2]]
	if !ok {
		return Range{}, fmt.Errorf("unknown size unit %q", m[2])
	}
	f, _ := strconv.ParseFloat(m[1], 64)
	n := int64(f * float64(unit))
	return Range{Lo: n, Hi: n}, nil
}

var relativePattern = regexp.MustCompile(`^(?:last|past)(\d+)(day|week|month|year)s?$`)

func dateParser(now time.Time) valueParser {
	return func(s string) (Range, error) { return parseDateValue(s, now) }
}

// parseDateValue covers today, yesterday, thisweek, lastweek, thismonth,
// lastmonth, thisyear, lastyear, last<N>days/weeks/months/years, and
// absolute dates YYYY, YYYY-MM and YYYY-MM-DD.
func parseDateValue(s string, now time.Time) (Range, error) {
	s = strings.ToLower(strings.TrimSpace(s))
	loc := now.Location()
	y, m, d := now.Date()
	today := time.Date(y, m, d, 0, 0, 0, 0, loc)
	span := func(from, to time.Time) Range { return Range{Lo: from.Unix(), Hi: to.Unix() - 1} }
	monday := today.AddDate(0, 0, -((int(today.Weekday()) + 6) % 7))
	firstOfMonth := time.Date(y, m, 1, 0, 0, 0, 0, loc)
	firstOfYear := time.Date(y, 1, 1, 0, 0, 0, 0, loc)

	switch s {
	case "today":
		return span(today, today.AddDate(0, 0, 1)), nil
	case "yesterday":
		return span(today.AddDate(0, 0, -1), today), nil
	case "thisweek":
		return span(monday, monday.AddDate(0, 0, 7)), nil
	case "lastweek":
		return span(monday.AddDate(0, 0, -7), monday), nil
	case "thismonth":
		return span(firstOfMonth, firstOfMonth.AddDate(0, 1, 0)), nil
	case "lastmonth":
		return span(firstOfMonth.AddDate(0, -1, 0), firstOfMonth), nil
	case "thisyear":
		return span(firstOfYear, firstOfYear.AddDate(1, 0, 0)), nil
	case "lastyear":
		return span(firstOfYear.AddDate(-1, 0, 0), firstOfYear), nil
	}
	if m := relativePattern.FindStringSubmatch(s); m != nil {
		n, _ := strconv.Atoi(m[1])
		var from time.Time
		switch m[2] {
		case "day":
			from = now.AddDate(0, 0, -n)
		case "week":
			from = now.AddDate(0, 0, -7*n)
		case "month":
			from = now.AddDate(0, -n, 0)
		case "year":
			from = now.AddDate(-n, 0, 0)
		}
		return Range{Lo: from.Unix(), Hi: now.Unix()}, nil
	}
	return parseAbsoluteDate(s, loc)
}

func parseAbsoluteDate(s string, loc *time.Location) (Range, error) {
	parts := strings.FieldsFunc(s, func(r rune) bool { return r == '-' || r == '/' || r == '.' })
	if len(parts) == 0 || len(parts) > 3 {
		return Range{}, errors.New("expected a date such as 2024, 2024-03 or 2024-03-15")
	}
	nums := make([]int, len(parts))
	for i, p := range parts {
		n, err := strconv.Atoi(p)
		if err != nil {
			return Range{}, fmt.Errorf("unknown date %q", s)
		}
		nums[i] = n
	}
	if nums[0] < 1000 {
		return Range{}, fmt.Errorf("year must come first in %q", s)
	}
	start := time.Date(nums[0], 1, 1, 0, 0, 0, 0, loc)
	var end time.Time
	switch len(nums) {
	case 1:
		end = start.AddDate(1, 0, 0)
	case 2:
		start = time.Date(nums[0], time.Month(nums[1]), 1, 0, 0, 0, 0, loc)
		end = start.AddDate(0, 1, 0)
	case 3:
		start = time.Date(nums[0], time.Month(nums[1]), nums[2], 0, 0, 0, 0, loc)
		end = start.AddDate(0, 0, 1)
	}
	return Range{Lo: start.Unix(), Hi: end.Unix() - 1}, nil
}
