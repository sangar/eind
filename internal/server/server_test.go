package server

import (
	"bufio"
	"context"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"eind/internal/index"
)

func sampleIndex() *index.Index {
	root := filepath.Join(string(filepath.Separator), "data")
	ix := index.New([]string{root})
	r := ix.Add(index.Entry{Name: root, Parent: index.NoParent, IsDir: true})
	docs := ix.Add(index.Entry{Name: "docs", Parent: r, IsDir: true})
	ix.Add(index.Entry{Name: "old-report-notes.txt", Parent: docs, Size: 3})
	ix.Add(index.Entry{Name: "Report.pdf", Parent: docs, Size: 2048})
	ix.Add(index.Entry{Name: "reporting.md", Parent: r, Size: 5})
	ix.Add(index.Entry{Name: "other.go", Parent: r, Size: 1})
	return ix
}

func startServer(t *testing.T) net.Conn {
	t.Helper()
	dir, err := os.MkdirTemp("", "eind")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.RemoveAll(dir) })
	sock := filepath.Join(dir, "s")
	ln, err := Listen(sock)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		defer close(done)
		New(sampleIndex(), &sync.RWMutex{}, "/idx", os.Stderr).Serve(ctx, ln)
	}()
	t.Cleanup(func() { cancel(); <-done })
	conn, err := net.Dial("unix", sock)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn
}

func roundTrip(t *testing.T, conn net.Conn, req string) map[string]any {
	t.Helper()
	if _, err := conn.Write([]byte(req + "\n")); err != nil {
		t.Fatal(err)
	}
	return readResponse(t, bufio.NewReader(conn))
}

func readResponse(t *testing.T, r *bufio.Reader) map[string]any {
	t.Helper()
	line, err := r.ReadBytes('\n')
	if err != nil {
		t.Fatal(err)
	}
	var resp map[string]any
	if err := json.Unmarshal(line, &resp); err != nil {
		t.Fatalf("bad json %q: %v", line, err)
	}
	return resp
}

func names(resp map[string]any) []string {
	var out []string
	for _, r := range resp["results"].([]any) {
		out = append(out, r.(map[string]any)["name"].(string))
	}
	return out
}

func TestSearchRanksAndLimits(t *testing.T) {
	conn := startServer(t)
	resp := roundTrip(t, conn, `{"id":7,"query":"report"}`)
	if resp["id"].(float64) != 7 || resp["total"].(float64) != 3 {
		t.Fatalf("resp = %v", resp)
	}
	if got := names(resp); got[0] != "Report.pdf" || got[1] != "reporting.md" || got[2] != "old-report-notes.txt" {
		t.Errorf("ranking = %v", got)
	}
	resp = roundTrip(t, conn, `{"query":"report","limit":1,"offset":1,"sort":"size","descending":true}`)
	if got := names(resp); len(got) != 1 || got[0] != "reporting.md" || resp["total"].(float64) != 3 {
		t.Errorf("limit/offset/sort = %v (%v)", got, resp)
	}
	if _, ok := resp["id"]; ok {
		t.Error("id should be omitted when the request had none")
	}
}

func TestStatusAndErrors(t *testing.T) {
	conn := startServer(t)
	status := roundTrip(t, conn, `{"op":"status"}`)
	if status["files"].(float64) != 4 || status["folders"].(float64) != 2 || status["index"] != "/idx" {
		t.Errorf("status = %v", status)
	}
	if resp := roundTrip(t, conn, `{"query":"size:huge!"}`); resp["error"] == nil {
		t.Errorf("expected query error, got %v", resp)
	}
	if resp := roundTrip(t, conn, `{"op":"dance"}`); resp["error"] == nil {
		t.Errorf("expected op error, got %v", resp)
	}
	if resp := roundTrip(t, conn, `not json`); resp["error"] == nil {
		t.Errorf("expected parse error, got %v", resp)
	}
	if resp := roundTrip(t, conn, `{"query":"report","sort":"bogus"}`); resp["error"] == nil {
		t.Errorf("expected sort error, got %v", resp)
	}
}

func TestRapidRequestsAreAnsweredOrCancelled(t *testing.T) {
	conn := startServer(t)
	const n = 20
	for i := 0; i < n; i++ {
		if _, err := conn.Write([]byte(`{"id":` + string(rune('0'+i%10)) + `,"query":"report"}` + "\n")); err != nil {
			t.Fatal(err)
		}
	}
	conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	r := bufio.NewReader(conn)
	answered, cancelled := 0, 0
	for i := 0; i < n; i++ {
		resp := readResponse(t, r)
		switch {
		case resp["cancelled"] == true:
			cancelled++
		case resp["results"] != nil:
			answered++
		default:
			t.Fatalf("unexpected response %v", resp)
		}
	}
	if answered+cancelled != n || answered == 0 {
		t.Errorf("answered %d, cancelled %d", answered, cancelled)
	}
}

func TestListenReplacesStaleSocketButNotLiveOne(t *testing.T) {
	dir, _ := os.MkdirTemp("", "eind")
	defer os.RemoveAll(dir)
	sock := filepath.Join(dir, "s")
	os.WriteFile(sock, nil, 0o600)
	ln, err := Listen(sock)
	if err != nil {
		t.Fatalf("stale socket file should be replaced: %v", err)
	}
	defer ln.Close()
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			c.Close()
		}
	}()
	if _, err := Listen(sock); err == nil {
		t.Error("expected an error while another daemon is listening")
	}
	if !Running(sock) {
		t.Error("Running should report the live listener")
	}
}
