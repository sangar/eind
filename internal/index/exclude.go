package index

import (
	"os"
	"path/filepath"
	"strings"

	"github.com/bmatcuk/doublestar/v4"
)

// Excludes decides which paths stay out of the index.
//
// A pattern without a path separator is matched against the file name
// ("node_modules", "*.tmp"). A pattern with a separator is matched against
// the full path and also excludes everything below it ("/proc",
// "~/Library/Caches", "**/.git").
type Excludes struct {
	names []string
	paths []string
}

func NewExcludes(patterns []string) (*Excludes, error) {
	ex := &Excludes{}
	home, _ := os.UserHomeDir()
	for _, p := range patterns {
		p = strings.TrimSpace(p)
		if p == "" {
			continue
		}
		if p == "~" || strings.HasPrefix(p, "~/") {
			p = home + p[1:]
		}
		p = filepath.ToSlash(p)
		if !doublestar.ValidatePattern(p) {
			return nil, &InvalidPatternError{Pattern: p}
		}
		if strings.Contains(p, "/") {
			ex.paths = append(ex.paths, strings.TrimSuffix(p, "/"))
		} else {
			ex.names = append(ex.names, p)
		}
	}
	return ex, nil
}

type InvalidPatternError struct{ Pattern string }

func (e *InvalidPatternError) Error() string { return "invalid exclude pattern: " + e.Pattern }

func (ex *Excludes) Empty() bool { return len(ex.names) == 0 && len(ex.paths) == 0 }

func (ex *Excludes) Match(path, name string) bool {
	for _, p := range ex.names {
		if ok, _ := doublestar.Match(p, name); ok {
			return true
		}
	}
	if len(ex.paths) == 0 {
		return false
	}
	slashed := filepath.ToSlash(path)
	for _, p := range ex.paths {
		if ok, _ := doublestar.Match(p, slashed); ok {
			return true
		}
		if ok, _ := doublestar.Match(p+"/**", slashed); ok {
			return true
		}
	}
	return false
}
