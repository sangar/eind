package main

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

var binary string

func TestMain(m *testing.M) {
	dir, err := os.MkdirTemp("", "eind-bin")
	if err != nil {
		panic(err)
	}
	binary = filepath.Join(dir, "eind")
	if out, err := exec.Command("go", "build", "-o", binary, ".").CombinedOutput(); err != nil {
		panic(string(out))
	}
	code := m.Run()
	os.RemoveAll(dir)
	os.Exit(code)
}

type fixture struct {
	root, config, index string
}

func newFixture(t *testing.T) fixture {
	t.Helper()
	base := t.TempDir()
	fx := fixture{root: filepath.Join(base, "root"), config: filepath.Join(base, "config"), index: filepath.Join(base, "index.bin")}
	files := map[string]int{
		"docs/report.txt":     10,
		"docs/notes.md":       10,
		"docs/big.pdf":        3000,
		"src/main.rs":         10,
		"src/pkg/util.go":     10,
		"Photos/IMG_001.JPG":  10,
		"build/out.o":         10,
		"My Notes/todo do.md": 10,
	}
	for rel, size := range files {
		p := filepath.Join(fx.root, rel)
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, make([]byte, size), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(fx.config, []byte("root = "+fx.root+"\nexclude = build\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if out, err := fx.run(t, "index"); err != nil {
		t.Fatalf("index: %v\n%s", err, out)
	}
	return fx
}

func (fx fixture) run(t *testing.T, args ...string) (string, error) {
	t.Helper()
	cmd := exec.Command(binary, args...)
	cmd.Env = append(os.Environ(), "EIND_CONFIG="+fx.config, "EIND_INDEX="+fx.index, "NO_COLOR=1")
	out, err := cmd.CombinedOutput()
	return string(out), err
}

func (fx fixture) lines(t *testing.T, args ...string) []string {
	t.Helper()
	out, err := fx.run(t, args...)
	if err != nil {
		t.Fatalf("%v: %v\n%s", args, err, out)
	}
	out = strings.TrimSpace(out)
	if out == "" {
		return nil
	}
	var rel []string
	for _, l := range strings.Split(out, "\n") {
		rel = append(rel, strings.TrimPrefix(l, fx.root+string(filepath.Separator)))
	}
	return rel
}

func TestSearchFromTheCommandLine(t *testing.T) {
	fx := newFixture(t)
	cases := []struct {
		args []string
		want []string
	}{
		{[]string{"report"}, []string{"docs/report.txt"}},
		{[]string{"ext:md", "-s", "name"}, []string{"docs/notes.md", "My Notes/todo do.md"}},
		{[]string{"out.o"}, nil},
		{[]string{"size:>1kb"}, []string{"docs/big.pdf"}},
		{[]string{"-r", `^img_\d+`}, []string{"Photos/IMG_001.JPG"}},
		{[]string{"todo", "do", "-n", "1", "--name-only"}, []string{"todo do.md"}},
		{[]string{"--path", filepath.Join(fx.root, "src"), "--files"}, []string{"src/main.rs", "src/pkg/util.go"}},
		{[]string{"--dirs", "-s", "name", "-d", "-n", "2"}, []string{"src", "src/pkg"}},
		{[]string{"--count", "file:"}, []string{"7"}},
		{[]string{"--", "index"}, nil},
	}
	for _, c := range cases {
		got := fx.lines(t, c.args...)
		if strings.Join(got, "|") != strings.Join(c.want, "|") {
			t.Errorf("%v: got %v, want %v", c.args, got, c.want)
		}
	}
}

func TestStructuredOutput(t *testing.T) {
	fx := newFixture(t)
	out, err := fx.run(t, "--json", "big")
	if err != nil {
		t.Fatal(err, out)
	}
	var records []struct {
		Path, Name, Type string
		Size             int64
	}
	if err := json.Unmarshal([]byte(out), &records); err != nil {
		t.Fatalf("bad json: %v\n%s", err, out)
	}
	if len(records) != 1 || records[0].Name != "big.pdf" || records[0].Size != 3000 || records[0].Type != "file" {
		t.Errorf("records = %+v", records)
	}

	out, _ = fx.run(t, "--csv", "-s", "name", "ext:txt")
	if lines := strings.Split(strings.TrimSpace(out), "\n"); len(lines) != 2 || !strings.HasPrefix(lines[0], "path,name,type,size") {
		t.Errorf("csv = %q", out)
	}

	out, _ = fx.run(t, "-0", "ext:txt")
	if !strings.HasSuffix(out, "report.txt\x00") {
		t.Errorf("null-separated = %q", out)
	}

	out, _ = fx.run(t, "--size", "--dm", "big")
	if !strings.HasPrefix(out, "   2.9 KB  ") || !strings.Contains(out, "docs/big.pdf") {
		t.Errorf("columns = %q", out)
	}
}

func TestStatusAndConfig(t *testing.T) {
	fx := newFixture(t)
	out, err := fx.run(t, "status")
	if err != nil || !strings.Contains(out, "7 files, 6 folders") || !strings.Contains(out, "root:   "+fx.root) {
		t.Errorf("status: %v\n%s", err, out)
	}
	out, err = fx.run(t, "config")
	if err != nil || !strings.Contains(out, "root = "+fx.root) || !strings.Contains(out, "exclude = build") {
		t.Errorf("config: %v\n%s", err, out)
	}
}

func TestBadInputIsReported(t *testing.T) {
	fx := newFixture(t)
	out, err := fx.run(t, "size:huge!")
	if err == nil || !strings.Contains(out, "eind: size:") {
		t.Errorf("expected size error, got %v\n%s", err, out)
	}
	out, err = fx.run(t, "--bogus")
	if err == nil || !strings.Contains(out, "see eind --help") {
		t.Errorf("expected flag error, got %v\n%s", err, out)
	}
}
