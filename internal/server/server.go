// Package server answers search requests over a Unix socket so that GUIs and
// launchers get results from the in-memory index in a few milliseconds.
//
// The protocol is JSON lines: one request object per line, one response
// object per request. A new request on a connection cancels the one before
// it, which then answers {"id":..,"cancelled":true}.
//
//	{"id":1,"query":"report ext:pdf","limit":50}
//	{"id":1,"total":132,"elapsed_ms":2.1,"results":[{"path":"/home/me/x/report.pdf","name":"report.pdf","type":"file","size":1024,"modified":"2024-03-13T10:00:00+01:00"}]}
//	{"op":"status"}
//	{"files":1847170,"folders":330914,"roots":["/home/me"],"built":"2024-03-13T09:00:00+01:00","index":"/home/me/.local/share/eind/index.bin"}
package server

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"sync"
	"time"

	"eind/internal/index"
	"eind/internal/output"
	"eind/internal/query"
	"eind/internal/search"
)

const (
	DefaultLimit   = 100
	maxRequestLine = 1 << 20
)

// DefaultSocketPath is $XDG_RUNTIME_DIR/eind.sock when available, otherwise a
// per-user file in the temp directory. EIND_SOCKET overrides it.
func DefaultSocketPath() string {
	if p := os.Getenv("EIND_SOCKET"); p != "" {
		return p
	}
	if dir := os.Getenv("XDG_RUNTIME_DIR"); dir != "" {
		return filepath.Join(dir, "eind.sock")
	}
	return filepath.Join(os.TempDir(), fmt.Sprintf("eind-%d.sock", os.Getuid()))
}

// Running reports whether a daemon answers at the socket path.
func Running(path string) bool {
	conn, err := net.DialTimeout("unix", path, 200*time.Millisecond)
	if err != nil {
		return false
	}
	conn.Close()
	return true
}

type Request struct {
	ID         json.RawMessage `json:"id,omitempty"`
	Op         string          `json:"op,omitempty"` // "search" (default) or "status"
	Query      string          `json:"query"`
	Limit      *int            `json:"limit,omitempty"` // default 100; negative means unlimited
	Offset     int             `json:"offset,omitempty"`
	Sort       string          `json:"sort,omitempty"` // relevance (default), path, name, size, dm, dc, ext
	Descending bool            `json:"descending,omitempty"`
	Regex      bool            `json:"regex,omitempty"`
	Case       bool            `json:"case,omitempty"`
	WholeWord  bool            `json:"whole_word,omitempty"`
	MatchPath  bool            `json:"match_path,omitempty"`
	Path       string          `json:"path,omitempty"`  // only results below this folder
	Files      bool            `json:"files,omitempty"` // only files
	Dirs       bool            `json:"dirs,omitempty"`  // only folders
}

type SearchResponse struct {
	ID        json.RawMessage `json:"id,omitempty"`
	Total     int             `json:"total"`
	ElapsedMs float64         `json:"elapsed_ms"`
	Results   []output.Record `json:"results"`
}

type StatusResponse struct {
	ID      json.RawMessage `json:"id,omitempty"`
	Files   int             `json:"files"`
	Folders int             `json:"folders"`
	Roots   []string        `json:"roots"`
	Built   string          `json:"built"`
	Index   string          `json:"index"`
}

type ErrorResponse struct {
	ID    json.RawMessage `json:"id,omitempty"`
	Error string          `json:"error"`
}

type CancelledResponse struct {
	ID        json.RawMessage `json:"id,omitempty"`
	Cancelled bool            `json:"cancelled"`
}

type Server struct {
	ix        *index.Index
	mu        *sync.RWMutex
	indexPath string
	log       io.Writer
}

// New serves ix, taking mu's read lock for every request so the index may be
// updated concurrently under its write lock.
func New(ix *index.Index, mu *sync.RWMutex, indexPath string, log io.Writer) *Server {
	return &Server{ix: ix, mu: mu, indexPath: indexPath, log: log}
}

// maxSocketPath is the portable limit of sun_path (104 bytes on macOS and
// the BSDs, 108 on Linux) with room for the terminating NUL.
const maxSocketPath = 103

// Listen binds the socket, replacing a stale file left by a dead daemon.
func Listen(path string) (net.Listener, error) {
	if len(path) > maxSocketPath {
		return nil, fmt.Errorf("socket path %q is longer than %d bytes; choose a shorter --socket", path, maxSocketPath)
	}
	if _, err := os.Stat(path); err == nil {
		if Running(path) {
			return nil, fmt.Errorf("another eind daemon is already serving %s", path)
		}
		if err := os.Remove(path); err != nil {
			return nil, err
		}
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return nil, err
	}
	ln, err := net.Listen("unix", path)
	if err != nil {
		return nil, err
	}
	_ = os.Chmod(path, 0o600)
	return ln, nil
}

// Serve accepts connections until ctx is done, then closes the listener and
// every open connection, so shutdown never waits on an idle client.
func (s *Server) Serve(ctx context.Context, ln net.Listener) error {
	go func() {
		<-ctx.Done()
		ln.Close()
	}()
	var conns sync.WaitGroup
	defer conns.Wait()
	for {
		conn, err := ln.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			return err
		}
		conns.Add(1)
		go func() {
			defer conns.Done()
			s.handleConn(ctx, conn)
		}()
	}
}

func (s *Server) handleConn(ctx context.Context, conn net.Conn) {
	defer conn.Close()
	var writeMu sync.Mutex
	enc := json.NewEncoder(conn)
	send := func(v any) {
		writeMu.Lock()
		defer writeMu.Unlock()
		if err := enc.Encode(v); err != nil {
			conn.Close()
		}
	}
	connCtx, cancelConn := context.WithCancel(ctx)
	defer cancelConn()
	go func() {
		<-connCtx.Done()
		conn.Close()
	}()
	cancelPrevious := func() {}
	var inflight sync.WaitGroup
	defer inflight.Wait()

	sc := bufio.NewScanner(conn)
	sc.Buffer(make([]byte, 0, 64<<10), maxRequestLine)
	for sc.Scan() {
		line := bytes.TrimSpace(sc.Bytes())
		if len(line) == 0 {
			continue
		}
		var req Request
		if err := json.Unmarshal(line, &req); err != nil {
			send(ErrorResponse{Error: "invalid request: " + err.Error()})
			continue
		}
		cancelPrevious()
		reqCtx, cancel := context.WithCancel(connCtx)
		cancelPrevious = cancel
		inflight.Add(1)
		go func() {
			defer inflight.Done()
			s.handle(reqCtx, req, send)
		}()
	}
	cancelPrevious()
}

func (s *Server) handle(ctx context.Context, req Request, send func(any)) {
	switch req.Op {
	case "", "search":
		s.search(ctx, req, send)
	case "status":
		s.mu.RLock()
		files, dirs := s.ix.Stats()
		resp := StatusResponse{ID: req.ID, Files: files, Folders: dirs, Roots: s.ix.Roots, Built: s.ix.BuiltAt.Format(time.RFC3339), Index: s.indexPath}
		s.mu.RUnlock()
		send(resp)
	default:
		send(ErrorResponse{ID: req.ID, Error: fmt.Sprintf("unknown op %q", req.Op)})
	}
}

func (s *Server) search(ctx context.Context, req Request, send func(any)) {
	start := time.Now()
	sortKey := search.SortRelevance
	if req.Sort != "" {
		var err error
		if sortKey, err = search.ParseSortKey(req.Sort); err != nil {
			send(ErrorResponse{ID: req.ID, Error: err.Error()})
			return
		}
	}
	node, err := query.Parse(req.Query, query.Defaults{Regex: req.Regex, CaseSensitive: req.Case, WholeWord: req.WholeWord, MatchPath: req.MatchPath})
	if err != nil {
		send(ErrorResponse{ID: req.ID, Error: err.Error()})
		return
	}
	node = query.Restrict(node, req.Path, req.Files, req.Dirs)

	s.mu.RLock()
	defer s.mu.RUnlock()
	hits, err := search.RunContext(ctx, s.ix, node)
	if errors.Is(err, context.Canceled) {
		send(CancelledResponse{ID: req.ID, Cancelled: true})
		return
	}
	if err != nil {
		send(ErrorResponse{ID: req.ID, Error: err.Error()})
		return
	}
	total := len(hits)
	offset := min(max(req.Offset, 0), total)
	limit := DefaultLimit
	if req.Limit != nil {
		limit = *req.Limit
	}
	if limit == 0 {
		send(SearchResponse{ID: req.ID, Total: total, ElapsedMs: float64(time.Since(start).Microseconds()) / 1000, Results: []output.Record{}})
		return
	}
	if sortKey == search.SortRelevance {
		keep := -1
		if limit >= 0 {
			keep = offset + limit
		}
		hits = search.Rank(s.ix, hits, node, keep)
	} else {
		search.Sort(s.ix, hits, sortKey, req.Descending)
	}
	if ctx.Err() != nil {
		send(CancelledResponse{ID: req.ID, Cancelled: true})
		return
	}
	hits = hits[min(offset, len(hits)):]
	if limit >= 0 && limit < len(hits) {
		hits = hits[:limit]
	}
	results := make([]output.Record, 0, len(hits))
	for _, h := range hits {
		results = append(results, output.RecordOf(s.ix, h))
	}
	send(SearchResponse{ID: req.ID, Total: total, ElapsedMs: float64(time.Since(start).Microseconds()) / 1000, Results: results})
}
