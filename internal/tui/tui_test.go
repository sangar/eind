package tui

import (
	"fmt"
	"path/filepath"
	"testing"

	"github.com/gdamore/tcell/v2"

	"eind/internal/index"
	"eind/internal/query"
)

func TestTypingFiltersAndEnterReturnsPath(t *testing.T) {
	root := filepath.Join(string(filepath.Separator), "data")
	ix := index.New([]string{root})
	r := ix.Add(index.Entry{Name: root, Parent: index.NoParent, IsDir: true})
	ix.Add(index.Entry{Name: "alpha.txt", Parent: r})
	ix.Add(index.Entry{Name: "beta.txt", Parent: r})
	ix.Add(index.Entry{Name: "beta.md", Parent: r})

	screen := tcell.NewSimulationScreen("UTF-8")
	if err := screen.Init(); err != nil {
		t.Fatal(err)
	}
	screen.SetSize(80, 24)
	go func() {
		for _, r := range "beta ext:md" {
			screen.InjectKey(tcell.KeyRune, r, tcell.ModNone)
		}
		screen.InjectKey(tcell.KeyEnter, 0, tcell.ModNone)
	}()

	got, err := run(screen, ix, query.Defaults{})
	if err != nil {
		t.Fatal(err)
	}
	if want := filepath.Join(root, "beta.md"); got != want {
		t.Errorf("got %q, want %q", got, want)
	}
}

func TestEscapeReturnsNothing(t *testing.T) {
	ix := index.New(nil)
	screen := tcell.NewSimulationScreen("UTF-8")
	if err := screen.Init(); err != nil {
		t.Fatal(err)
	}
	screen.InjectKey(tcell.KeyEscape, 0, tcell.ModNone)
	got, err := run(screen, ix, query.Defaults{})
	if err != nil || got != "" {
		t.Errorf("got %q, %v", got, err)
	}
}

func TestEnterWhileTypingWaitsForTheFinalResults(t *testing.T) {
	root := filepath.Join(string(filepath.Separator), "data")
	ix := index.New([]string{root})
	r := ix.Add(index.Entry{Name: root, Parent: index.NoParent, IsDir: true})
	for i := 0; i < 20000; i++ {
		ix.Add(index.Entry{Name: fmt.Sprintf("file%05d.txt", i), Parent: r})
	}
	ix.Add(index.Entry{Name: "zz-target.md", Parent: r})

	screen := tcell.NewSimulationScreen("UTF-8")
	if err := screen.Init(); err != nil {
		t.Fatal(err)
	}
	go func() {
		for _, r := range "zz-target" {
			screen.InjectKey(tcell.KeyRune, r, tcell.ModNone)
		}
		screen.InjectKey(tcell.KeyEnter, 0, tcell.ModNone)
	}()
	got, err := run(screen, ix, query.Defaults{})
	if err != nil {
		t.Fatal(err)
	}
	if want := filepath.Join(root, "zz-target.md"); got != want {
		t.Errorf("got %q, want %q", got, want)
	}
}
