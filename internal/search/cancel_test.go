package search

import (
	"context"
	"testing"

	"eind/internal/query"
)

func TestCancelledSearchReturnsContextError(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := RunContext(ctx, sample(), query.MatchAll); err != context.Canceled {
		t.Errorf("got %v, want context.Canceled", err)
	}
}
