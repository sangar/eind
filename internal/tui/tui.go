// Package tui is the interactive search-as-you-type view.
package tui

import (
	"context"
	"fmt"
	"os/exec"
	"runtime"
	"strings"
	"time"

	"github.com/gdamore/tcell/v2"

	"eind/internal/index"
	"eind/internal/output"
	"eind/internal/query"
	"eind/internal/search"
)

// typingPause is how long the input must be idle before a search starts.
const typingPause = 40 * time.Millisecond

// Run opens the interactive view and returns the path the user accepted with
// Enter, or "" if they quit.
func Run(ix *index.Index, defaults query.Defaults) (string, error) {
	screen, err := tcell.NewScreen()
	if err != nil {
		return "", err
	}
	if err := screen.Init(); err != nil {
		return "", err
	}
	defer screen.Fini()
	return run(screen, ix, defaults)
}

type result struct {
	generation int
	hits       []uint32
	err        error
	elapsed    time.Duration
}

// run keeps the event loop free: searches happen on their own goroutine and
// are cancelled as soon as the input changes again.
func run(screen tcell.Screen, ix *index.Index, defaults query.Defaults) (string, error) {
	screen.EnableMouse(tcell.MouseButtonEvents)
	events := make(chan tcell.Event, 64)
	quit := make(chan struct{})
	go screen.ChannelEvents(events, quit)
	defer close(quit)

	v := &view{ix: ix, defaults: defaults, screen: screen, results: make(chan result, 1)}
	defer v.cancelSearch()
	v.startSearch()
	var settled <-chan time.Time
	for {
		v.draw()
		select {
		case ev := <-events:
			action := v.handle(ev)
			switch action {
			case actQuit:
				return "", nil
			case actAccept:
				if !v.searching {
					return v.selectedPath(), nil
				}
				v.acceptWhenDone = true
			case actInputChanged:
				v.cancelSearch()
				v.searching = true
				settled = time.After(typingPause)
			}
		case <-settled:
			settled = nil
			v.startSearch()
		case r := <-v.results:
			if r.generation != v.generation {
				continue
			}
			v.apply(r)
			if v.acceptWhenDone {
				return v.selectedPath(), nil
			}
		}
	}
}

type action int

const (
	actNone action = iota
	actQuit
	actAccept
	actInputChanged
)

type view struct {
	ix       *index.Index
	defaults query.Defaults
	screen   tcell.Screen

	input    []rune
	hits     []uint32
	selected int
	top      int
	elapsed  time.Duration
	err      string

	results        chan result
	generation     int
	cancel         context.CancelFunc
	searching      bool
	acceptWhenDone bool
}

func (v *view) cancelSearch() {
	if v.cancel != nil {
		v.cancel()
		v.cancel = nil
	}
}

func (v *view) startSearch() {
	v.cancelSearch()
	v.generation++
	v.searching = true
	ctx, cancel := context.WithCancel(context.Background())
	v.cancel = cancel
	go runSearch(ctx, v.ix, string(v.input), v.defaults, v.generation, v.results)
}

func runSearch(ctx context.Context, ix *index.Index, input string, defaults query.Defaults, generation int, results chan<- result) {
	start := time.Now()
	r := result{generation: generation}
	node, err := query.Parse(input, defaults)
	if err == nil {
		r.hits, err = search.RunContext(ctx, ix, node)
	}
	if err != nil {
		r.err = err
	} else if strings.TrimSpace(input) != "" {
		// A blank query lists the whole index; leaving it
		// in index order keeps that instant even for millions of entries.
		search.Sort(ix, r.hits, search.SortName, false)
	}
	r.elapsed = time.Since(start)
	select {
	case results <- r:
	case <-ctx.Done():
	}
}

func (v *view) apply(r result) {
	v.searching = false
	v.cancel = nil
	v.elapsed = r.elapsed
	v.selected, v.top = 0, 0
	if r.err != nil {
		v.err = r.err.Error()
		v.hits = nil
		return
	}
	v.err = ""
	v.hits = r.hits
}

func (v *view) selectedPath() string {
	if len(v.hits) == 0 {
		return ""
	}
	return v.ix.Path(v.hits[v.selected])
}

func (v *view) handle(ev tcell.Event) action {
	switch ev := ev.(type) {
	case *tcell.EventResize:
		v.screen.Sync()
	case *tcell.EventMouse:
		v.mouse(ev)
	case *tcell.EventKey:
		return v.key(ev)
	}
	return actNone
}

func (v *view) key(ev *tcell.EventKey) action {
	_, height := v.screen.Size()
	page := max(height-3, 1)
	switch ev.Key() {
	case tcell.KeyEscape, tcell.KeyCtrlC:
		return actQuit
	case tcell.KeyEnter:
		return actAccept
	case tcell.KeyCtrlO:
		if p := v.selectedPath(); p != "" {
			openWithSystem(p)
		}
	case tcell.KeyBackspace, tcell.KeyBackspace2:
		if len(v.input) > 0 {
			v.input = v.input[:len(v.input)-1]
			return actInputChanged
		}
	case tcell.KeyCtrlU:
		if len(v.input) > 0 {
			v.input = v.input[:0]
			return actInputChanged
		}
	case tcell.KeyCtrlW:
		if len(v.input) > 0 {
			v.input = deleteWord(v.input)
			return actInputChanged
		}
	case tcell.KeyDown, tcell.KeyCtrlN:
		v.move(1)
	case tcell.KeyUp, tcell.KeyCtrlP:
		v.move(-1)
	case tcell.KeyPgDn:
		v.move(page)
	case tcell.KeyPgUp:
		v.move(-page)
	case tcell.KeyHome:
		v.move(-len(v.hits))
	case tcell.KeyEnd:
		v.move(len(v.hits))
	case tcell.KeyRune:
		v.input = append(v.input, ev.Rune())
		return actInputChanged
	}
	return actNone
}

func (v *view) mouse(ev *tcell.EventMouse) {
	switch ev.Buttons() {
	case tcell.WheelDown:
		v.move(3)
	case tcell.WheelUp:
		v.move(-3)
	case tcell.Button1:
		_, y := ev.Position()
		if row := v.top + y - 2; y >= 2 && row < len(v.hits) {
			v.selected = row
		}
	}
}

func (v *view) move(delta int) {
	if len(v.hits) == 0 {
		return
	}
	v.selected = min(max(v.selected+delta, 0), len(v.hits)-1)
}

func deleteWord(input []rune) []rune {
	i := len(input)
	for i > 0 && input[i-1] == ' ' {
		i--
	}
	for i > 0 && input[i-1] != ' ' {
		i--
	}
	return input[:i]
}

func (v *view) draw() {
	s := v.screen
	width, height := s.Size()
	s.Clear()

	prompt := "> "
	putString(s, 0, 0, prompt+string(v.input), tcell.StyleDefault.Bold(true))
	s.ShowCursor(len(prompt)+len(v.input), 0)

	listTop, listHeight := 2, height-3
	if listHeight < 1 {
		return
	}
	if v.selected < v.top {
		v.top = v.selected
	}
	if v.selected >= v.top+listHeight {
		v.top = v.selected - listHeight + 1
	}
	for row := 0; row < listHeight; row++ {
		i := v.top + row
		if i >= len(v.hits) {
			break
		}
		v.drawRow(listTop+row, width, v.hits[i], i == v.selected)
	}

	status := fmt.Sprintf(" %s of %s objects  %s", withCommas(len(v.hits)), withCommas(v.ix.Len()), v.elapsed.Round(time.Millisecond))
	if v.searching {
		status = fmt.Sprintf(" %s of %s objects  searching...", withCommas(len(v.hits)), withCommas(v.ix.Len()))
	}
	if v.err != "" {
		status = " " + v.err
	}
	help := "Enter print path  Ctrl-O open  Esc quit "
	statusStyle := tcell.StyleDefault.Reverse(true)
	putString(s, 0, height-1, padRight(status, width-len(help))+help, statusStyle)
	s.Show()
}

func (v *view) drawRow(y, width int, hit uint32, selected bool) {
	e := v.ix.Entry(hit)
	style := tcell.StyleDefault
	if selected {
		style = style.Reverse(true)
	}
	nameStyle := style
	if e.IsDir {
		nameStyle = nameStyle.Foreground(tcell.ColorBlue).Bold(true)
	}
	dir := ""
	if e.Parent != index.NoParent {
		dir = v.ix.Path(e.Parent)
	}
	meta := formatMeta(&e)
	metaWidth := len(meta) + 1
	if selected {
		putString(v.screen, 0, y, strings.Repeat(" ", width), style)
	}
	x := putString(v.screen, 0, y, " "+e.Name, nameStyle)
	if dir != "" {
		available := width - x - metaWidth - 2
		if available > 4 {
			putString(v.screen, x, y, "  "+truncate(dir, available), style.Dim(true))
		}
	}
	putString(v.screen, width-metaWidth, y, meta, style.Dim(true))
}

func formatMeta(e *index.Entry) string {
	when := time.Unix(e.Modified, 0).Format("2006-01-02 15:04")
	if e.IsDir {
		return fmt.Sprintf("%9s  %s", "", when)
	}
	return fmt.Sprintf("%9s  %s", output.HumanSize(e.Size), when)
}

func putString(s tcell.Screen, x, y int, text string, style tcell.Style) int {
	width, _ := s.Size()
	for _, r := range text {
		if x >= width {
			break
		}
		s.SetContent(x, y, r, nil, style)
		x++
	}
	return x
}

func truncate(s string, width int) string {
	runes := []rune(s)
	if len(runes) <= width {
		return s
	}
	return "…" + string(runes[len(runes)-width+1:])
}

func padRight(s string, width int) string {
	if len(s) >= width {
		return s
	}
	return s + strings.Repeat(" ", width-len(s))
}

func withCommas(n int) string {
	s := fmt.Sprint(n)
	for i := len(s) - 3; i > 0; i -= 3 {
		s = s[:i] + "," + s[i:]
	}
	return s
}

func openWithSystem(path string) {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "darwin":
		cmd = exec.Command("open", path)
	case "windows":
		cmd = exec.Command("cmd", "/c", "start", "", path)
	default:
		cmd = exec.Command("xdg-open", path)
	}
	if err := cmd.Start(); err == nil {
		go func() { _ = cmd.Wait() }()
	}
}
