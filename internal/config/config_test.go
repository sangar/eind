package config

import (
	"os"
	"path/filepath"
	"reflect"
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
