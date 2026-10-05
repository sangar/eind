// Package watch keeps the index current from filesystem change notifications.
package watch

import (
	"context"
	"errors"
	"fmt"
	"io"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/rjeczalik/notify"

	"eind/internal/index"
	"eind/internal/update"
)

const settleDelay = 250 * time.Millisecond

// Run watches every root of the index and records each change in the index
// file's journal as soon as it is applied, so that commands loading the index
// see it at once. Every saveInterval it folds a long journal into a new index
// file, and it does so once more when ctx is done. Every mutation of the
// index happens under mu's write lock, so readers such as the socket server
// can share the index safely.
func Run(ctx context.Context, ix *index.Index, ex *index.Excludes, indexPath string, saveInterval time.Duration, mu *sync.RWMutex, log io.Writer) error {
	if errUnsupported != nil {
		return errUnsupported
	}
	events := make(chan notify.EventInfo, 4096)
	for _, root := range ix.Roots {
		if err := notify.Watch(filepath.Join(root, "..."), events, notify.All); err != nil {
			return fmt.Errorf("watch %s: %w", root, err)
		}
		fmt.Fprintf(log, "watching %s\n", root)
	}
	defer notify.Stop(events)
	if err := ix.EnableJournal(indexPath); err != nil {
		return err
	}

	roots := resolveRoots(ix.Roots)
	updater := update.New(ix, ex)
	compact := func(force bool) error {
		mu.Lock()
		defer mu.Unlock()
		changes := ix.JournalEntries()
		if changes == 0 || !force && changes < compactAfter(ix) {
			return nil
		}
		if err := ix.Save(indexPath); err != nil {
			return err
		}
		updater.Rebuild()
		fmt.Fprintf(log, "%s folded %d changes into the index: %d entries\n", time.Now().Format("15:04:05"), changes, ix.Len())
		return nil
	}
	flush := func() error {
		err := ix.Flush()
		if !errors.Is(err, index.ErrReplaced) {
			return err
		}
		fmt.Fprintf(log, "%s %v; loading it again\n", time.Now().Format("15:04:05"), err)
		if err := ix.Reload(indexPath); err != nil {
			return err
		}
		if err := ix.EnableJournal(indexPath); err != nil {
			return err
		}
		updater.Rebuild()
		return nil
	}

	compactTimer := time.NewTicker(saveInterval)
	defer compactTimer.Stop()
	for {
		select {
		case <-ctx.Done():
			return compact(true)
		case <-compactTimer.C:
			if err := compact(false); err != nil {
				return err
			}
		case ev := <-events:
			changed := map[string]bool{roots.indexPath(ev.Path()): true}
			collectBurst(events, roots, changed)
			mu.Lock()
			for p := range changed {
				updater.Reconcile(p)
			}
			err := flush()
			mu.Unlock()
			if err != nil {
				return err
			}
		}
	}
}

// compactAfter is how many journaled changes make a new index file worth
// writing. Each command that loads the index replays the journal, which
// costs about a millisecond per ten thousand changes.
func compactAfter(ix *index.Index) int {
	return max(10_000, ix.Len()/100)
}

// collectBurst drains events that arrive close together so that a path touched
// many times in a row is re-examined once.
func collectBurst(events <-chan notify.EventInfo, roots rootPaths, changed map[string]bool) {
	deadline := time.After(2 * time.Second)
	for {
		select {
		case ev := <-events:
			changed[roots.indexPath(ev.Path())] = true
		case <-time.After(settleDelay):
			return
		case <-deadline:
			return
		}
	}
}

// rootPaths pairs each root with its path after resolving symlinks, which is
// how FSEvents reports changes: below a root of /var/data, a change arrives
// as /private/var/data/x.
type rootPaths []struct{ resolved, root string }

func resolveRoots(roots []string) rootPaths {
	var out rootPaths
	for _, root := range roots {
		root = strings.TrimSuffix(root, string(filepath.Separator))
		if resolved, err := filepath.EvalSymlinks(root); err == nil && resolved != root {
			out = append(out, struct{ resolved, root string }{resolved, root})
		}
	}
	return out
}

// indexPath turns a reported path into the path the index knows it by.
func (r rootPaths) indexPath(path string) string {
	for _, p := range r {
		if rest, ok := strings.CutPrefix(path, p.resolved); ok && (rest == "" || rest[0] == filepath.Separator) {
			return p.root + rest
		}
	}
	return path
}

// Supported reports why this build cannot watch the filesystem, if it cannot.
func Supported() error { return errUnsupported }

// ServiceHint explains how to keep `eind watch` running in the background.
func ServiceHint() string {
	return "Tip: `eind service enable` keeps it running in the background from login."
}
