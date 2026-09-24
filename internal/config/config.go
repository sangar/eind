// Package config locates and reads eind's configuration and index files.
//
// The config file is a plain list of "key = value" lines:
//
//	root = /Users/me
//	root = /Volumes/Data
//	exclude = /proc
//	exclude = **/node_modules
package config

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

type Config struct {
	Roots    []string
	Excludes []string
}

func Default() Config {
	home, err := os.UserHomeDir()
	if err != nil {
		home = "."
	}
	return Config{
		Roots: []string{home},
		Excludes: []string{
			"/proc", "/sys", "/dev", "/run",
			"/System/Volumes", "/Volumes", "/private/var/vm",
		},
	}
}

func ConfigPath() string {
	if p := os.Getenv("EIND_CONFIG"); p != "" {
		return p
	}
	return filepath.Join(configDir(), "eind", "config")
}

func IndexPath() string {
	if p := os.Getenv("EIND_INDEX"); p != "" {
		return p
	}
	return filepath.Join(dataDir(), "eind", "index.bin")
}

func configDir() string {
	if p := os.Getenv("XDG_CONFIG_HOME"); p != "" {
		return p
	}
	if runtime.GOOS == "windows" {
		if p, err := os.UserConfigDir(); err == nil {
			return p
		}
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".config")
}

func dataDir() string {
	if p := os.Getenv("XDG_DATA_HOME"); p != "" {
		return p
	}
	if runtime.GOOS == "windows" {
		if p := os.Getenv("LOCALAPPDATA"); p != "" {
			return p
		}
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".local", "share")
}

// Load reads the config file, falling back to Default when it does not exist.
func Load(path string) (Config, error) {
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return Default(), nil
	}
	if err != nil {
		return Config{}, err
	}
	defer f.Close()
	return parse(f)
}

func parse(f *os.File) (Config, error) {
	var cfg Config
	sc := bufio.NewScanner(f)
	line := 0
	for sc.Scan() {
		line++
		text := strings.TrimSpace(sc.Text())
		if text == "" || strings.HasPrefix(text, "#") {
			continue
		}
		key, value, ok := strings.Cut(text, "=")
		if !ok {
			return cfg, fmt.Errorf("%s:%d: expected key = value", f.Name(), line)
		}
		key, value = strings.TrimSpace(key), strings.TrimSpace(value)
		switch key {
		case "root":
			cfg.Roots = append(cfg.Roots, expandHome(value))
		case "exclude":
			cfg.Excludes = append(cfg.Excludes, value)
		default:
			return cfg, fmt.Errorf("%s:%d: unknown key %q", f.Name(), line, key)
		}
	}
	if err := sc.Err(); err != nil {
		return cfg, err
	}
	if len(cfg.Roots) == 0 {
		cfg.Roots = Default().Roots
	}
	return cfg, nil
}

func expandHome(p string) string {
	if p == "~" || strings.HasPrefix(p, "~/") {
		home, err := os.UserHomeDir()
		if err == nil {
			return home + p[1:]
		}
	}
	return p
}

// Render writes a config back in the file format, with a short syntax primer.
func Render(cfg Config) string {
	var b strings.Builder
	b.WriteString("# eind configuration\n")
	b.WriteString("#\n")
	b.WriteString("# root    = directory to index (repeat for several roots)\n")
	b.WriteString("# exclude = pattern to leave out. Without a slash it matches names\n")
	b.WriteString("#           (node_modules, *.tmp); with a slash it matches full paths\n")
	b.WriteString("#           and everything below (/proc, ~/Library/Caches, **/.git).\n")
	b.WriteString("#\n")
	b.WriteString("# Run `eind index` after changing this file.\n\n")
	for _, r := range cfg.Roots {
		fmt.Fprintf(&b, "root = %s\n", r)
	}
	b.WriteString("\n")
	for _, e := range cfg.Excludes {
		fmt.Fprintf(&b, "exclude = %s\n", e)
	}
	return b.String()
}

// WriteDefault creates the config file if it does not exist.
func WriteDefault(path string) (created bool, err error) {
	if _, err := os.Stat(path); err == nil {
		return false, nil
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return false, err
	}
	return true, os.WriteFile(path, []byte(Render(Default())), 0o644)
}
