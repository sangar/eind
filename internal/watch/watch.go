// Package watch keeps the index current from filesystem change notifications.
package watch

import (
	"context"
	"fmt"
	"io"
	"path/filepath"
	"sync"
	"time"

	"github.com/rjeczalik/notify"

	"eind/internal/index"
	"eind/internal/update"
)

const settleDelay = 250 * time.Millisecond

// Run watches every root of the index and saves it to indexPath whenever
// changes have accumulated for saveInterval. It returns when ctx is done,
// after a final save. Every mutation of the index happens under mu's write
// lock, so readers such as the socket server can share the index safely.
func Run(ctx context.Context, ix *index.Index, ex *index.Excludes, indexPath string, saveInterval time.Duration, mu *sync.RWMutex, log io.Writer) error {
	events := make(chan notify.EventInfo, 4096)
	for _, root := range ix.Roots {
		if err := notify.Watch(filepath.Join(root, "..."), events, notify.All); err != nil {
			return fmt.Errorf("watch %s: %w", root, err)
		}
		fmt.Fprintf(log, "watching %s\n", root)
	}
	defer notify.Stop(events)

	updater := update.New(ix, ex)
	dirty := false
	save := func() error {
		if !dirty {
			return nil
		}
		mu.Lock()
		defer mu.Unlock()
		before := len(ix.Entries)
		if err := ix.Save(indexPath); err != nil {
			return err
		}
		if len(ix.Entries) != before {
			updater.Rebuild()
		}
		dirty = false
		fmt.Fprintf(log, "%s saved index: %d entries\n", time.Now().Format("15:04:05"), ix.Len())
		return nil
	}

	saveTimer := time.NewTicker(saveInterval)
	defer saveTimer.Stop()
	for {
		select {
		case <-ctx.Done():
			return save()
		case <-saveTimer.C:
			if err := save(); err != nil {
				return err
			}
		case ev := <-events:
			changed := map[string]bool{ev.Path(): true}
			collectBurst(events, changed)
			mu.Lock()
			for p := range changed {
				if updater.Reconcile(p) {
					dirty = true
				}
			}
			mu.Unlock()
		}
	}
}

// collectBurst drains events that arrive close together so that a path touched
// many times in a row is re-examined once.
func collectBurst(events <-chan notify.EventInfo, changed map[string]bool) {
	deadline := time.After(2 * time.Second)
	for {
		select {
		case ev := <-events:
			changed[ev.Path()] = true
		case <-time.After(settleDelay):
			return
		case <-deadline:
			return
		}
	}
}

// ServiceHint explains how to keep `eind watch` running in the background.
func ServiceHint() string {
	return "Tip: `eind service enable` keeps it running in the background from login."
}
