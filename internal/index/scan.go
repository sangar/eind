package index

import (
	"os"
	"path/filepath"
	"runtime"
	"sync"
)

type ScanResult struct {
	Start, End uint32 // entries added, as a half-open index range
	Errors     int    // directories that could not be read
}

type dirJob struct {
	path string
	idx  uint32
}

type dirResult struct {
	parent   uint32
	path     string
	children []Entry
	failed   bool
}

// AddTree scans root and appends it, and everything below it, to the index.
// With parent == NoParent, root becomes a new index root; otherwise it is
// attached under the given directory entry. Directories are read in parallel,
// while entries are appended by a single collector so that parents always
// precede their children.
func (ix *Index) AddTree(root string, parent uint32, ex *Excludes, progress func(added int)) (ScanResult, error) {
	res := ScanResult{Start: uint32(len(ix.Entries))}
	info, err := os.Lstat(root)
	if err != nil {
		return res, err
	}
	name := root
	if parent != NoParent {
		name = filepath.Base(root)
	}
	rootIdx := ix.Add(entryFromInfo(name, parent, info))
	res.End = uint32(len(ix.Entries))
	if !info.IsDir() {
		return res, nil
	}

	workers := min(4*runtime.NumCPU(), 64)
	jobs := make(chan dirJob)
	results := make(chan dirResult, workers)
	var wg sync.WaitGroup
	for range workers {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for job := range jobs {
				results <- readDir(job, ex)
			}
		}()
	}

	pending := []dirJob{{path: root, idx: rootIdx}}
	inflight := 0
	lastReported := 0
	for len(pending) > 0 || inflight > 0 {
		var send chan<- dirJob
		var next dirJob
		if len(pending) > 0 {
			send = jobs
			next = pending[len(pending)-1]
		}
		select {
		case send <- next:
			pending = pending[:len(pending)-1]
			inflight++
		case r := <-results:
			inflight--
			if r.failed {
				res.Errors++
			}
			for _, child := range r.children {
				idx := ix.Add(child)
				if child.IsDir {
					pending = append(pending, dirJob{path: filepath.Join(r.path, child.Name), idx: idx})
				}
			}
			added := len(ix.Entries) - int(res.Start)
			if progress != nil && added-lastReported >= 10000 {
				lastReported = added
				progress(added)
			}
		}
	}
	close(jobs)
	wg.Wait()
	res.End = uint32(len(ix.Entries))
	return res, nil
}

func readDir(job dirJob, ex *Excludes) dirResult {
	res := dirResult{parent: job.idx, path: job.path}
	dirents, err := os.ReadDir(job.path)
	if err != nil {
		res.failed = true
	}
	res.children = make([]Entry, 0, len(dirents))
	for _, d := range dirents {
		name := d.Name()
		if ex != nil && !ex.Empty() && ex.Match(filepath.Join(job.path, name), name) {
			continue
		}
		info, err := d.Info()
		if err != nil {
			continue
		}
		res.children = append(res.children, entryFromInfo(name, job.idx, info))
	}
	return res
}

func entryFromInfo(name string, parent uint32, info os.FileInfo) Entry {
	e := Entry{
		Name:     name,
		Parent:   parent,
		Modified: info.ModTime().Unix(),
		Created:  birthTime(info),
		IsDir:    info.IsDir(),
	}
	if !e.IsDir {
		e.Size = info.Size()
	}
	return e
}

// EntryFromInfo builds an entry for a path that is already known to sit
// directly under parent.
func EntryFromInfo(name string, parent uint32, info os.FileInfo) Entry {
	return entryFromInfo(name, parent, info)
}
