package main

import (
	"encoding/json"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
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
	return fx.runWithEnv(t, nil, args...)
}

func (fx fixture) runWithEnv(t *testing.T, env []string, args ...string) (string, error) {
	t.Helper()
	cmd := exec.Command(binary, args...)
	cmd.Env = append(os.Environ(), "EIND_CONFIG="+fx.config, "EIND_INDEX="+fx.index, "NO_COLOR=1")
	cmd.Env = append(cmd.Env, env...)
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

func TestServeAnswersOverTheSocket(t *testing.T) {
	fx := newFixture(t)
	dir, err := os.MkdirTemp("", "eind")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(dir)
	sock := filepath.Join(dir, "s")
	cmd := exec.Command(binary, "serve", "--socket", sock)
	cmd.Env = append(os.Environ(), "EIND_CONFIG="+fx.config, "EIND_INDEX="+fx.index)
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { cmd.Process.Signal(os.Interrupt); cmd.Wait() }()

	var conn net.Conn
	for deadline := time.Now().Add(10 * time.Second); time.Now().Before(deadline); time.Sleep(50 * time.Millisecond) {
		if conn, err = net.Dial("unix", sock); err == nil {
			break
		}
	}
	if conn == nil {
		t.Fatalf("daemon never listened: %v", err)
	}
	defer conn.Close()

	if _, err := conn.Write([]byte(`{"id":1,"query":"report","limit":5}` + "\n")); err != nil {
		t.Fatal(err)
	}
	var resp struct {
		ID      int
		Total   int
		Results []struct{ Path, Name string }
	}
	if err := json.NewDecoder(conn).Decode(&resp); err != nil {
		t.Fatal(err)
	}
	if resp.ID != 1 || resp.Total != 1 || len(resp.Results) != 1 || resp.Results[0].Name != "report.txt" {
		t.Errorf("resp = %+v", resp)
	}
	out, _ := fx.runWithEnv(t, []string{"EIND_SOCKET=" + sock}, "status")
	if !strings.Contains(out, "daemon: running at "+sock) {
		t.Errorf("status should see the daemon:\n%s", out)
	}

	// With the index file gone, only the daemon can answer.
	if err := os.Remove(fx.index); err != nil {
		t.Fatal(err)
	}
	out, err = fx.runWithEnv(t, []string{"EIND_SOCKET=" + sock}, "report", "--count")
	if err != nil || strings.TrimSpace(out) != "1" {
		t.Errorf("count via daemon = %q, %v", out, err)
	}
	out, err = fx.runWithEnv(t, []string{"EIND_SOCKET=" + sock}, "--path", fx.root+"/docs", "--files", "--size", "-s", "size", "-d", ".md|.txt")
	if err != nil || !strings.Contains(out, "notes.md") || !strings.Contains(out, "report.txt") || !strings.Contains(out, "10 B") {
		t.Errorf("search via daemon = %q, %v", out, err)
	}
	out, err = fx.runWithEnv(t, []string{"EIND_SOCKET=" + sock}, "size:huge!")
	if err == nil || !strings.Contains(out, "size:") {
		t.Errorf("query errors should come back from the daemon: %q, %v", out, err)
	}
}

func TestServiceRejectsUnknownAction(t *testing.T) {
	fx := newFixture(t)
	out, err := fx.run(t, "service", "restart")
	if err == nil || !strings.Contains(out, "usage: eind service enable|disable") {
		t.Errorf("out = %q, err = %v", out, err)
	}
}
