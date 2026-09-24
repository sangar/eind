package config

import (
	"os"
	"path/filepath"
	"reflect"
	"runtime"
	"slices"
	"strings"
	"testing"
)

func load(t *testing.T, text string) Config {
	t.Helper()
	path := filepath.Join(t.TempDir(), "config")
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
	cfg, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	return cfg
}

func TestExcludesDefaultOnlyWhenTheFileNamesNone(t *testing.T) {
	if got := load(t, "root = /data\n").Excludes; !reflect.DeepEqual(got, DefaultExcludes()) {
		t.Errorf("no exclude lines should use the defaults, got %v", got)
	}
	if got := load(t, "root = /data\nexclude = *.tmp\n").Excludes; !reflect.DeepEqual(got, []string{"*.tmp"}) {
		t.Errorf("exclude lines should replace the defaults, got %v", got)
	}
	if got := load(t, "").Roots; len(got) != 1 {
		t.Errorf("roots should default to the home directory, got %v", got)
	}
}

func TestRenderedDefaultsRoundTrip(t *testing.T) {
	cfg := load(t, Render(Default()))
	if !reflect.DeepEqual(cfg, Default()) {
		t.Errorf("rendered defaults read back as %+v", cfg)
	}
}

func TestDefaultExcludesFitTheRunningPlatform(t *testing.T) {
	got := DefaultExcludes()
	for _, want := range []string{"node_modules", ".git", ".cache"} {
		if !slices.Contains(got, want) {
			t.Errorf("defaults %v lack %q", got, want)
		}
	}
	foreign := map[string][]string{
		"darwin": {"/proc", "/sys", "~/.local/share/Trash"},
		"linux":  {"/Volumes", "~/.Trash", "~/Library/Caches"},
	}
	for _, p := range foreign[runtime.GOOS] {
		if slices.Contains(got, p) {
			t.Errorf("defaults on %s should not mention %q", runtime.GOOS, p)
		}
	}
	for _, p := range got {
		if strings.HasPrefix(p, "/home/") || strings.HasPrefix(p, "/Users/") {
			t.Errorf("%q should be written relative to ~", p)
		}
	}
}
