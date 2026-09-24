// Package tui is the interactive search-as-you-type view.
package tui

import (
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

func run(screen tcell.Screen, ix *index.Index, defaults query.Defaults) (string, error) {
	screen.EnableMouse(tcell.MouseButtonEvents)
	v := &view{ix: ix, defaults: defaults, screen: screen}
	v.search()
	for {
		v.draw()
		switch ev := screen.PollEvent().(type) {
		case *tcell.EventResize:
			screen.Sync()
		case *tcell.EventMouse:
			v.mouse(ev)
		case *tcell.EventKey:
			if done, result := v.key(ev); done {
				return result, nil
			}
		}
	}
}

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
}

func (v *view) search() {
	start := time.Now()
	node, err := query.Parse(string(v.input), v.defaults)
	if err == nil {
		v.hits, err = search.Run(v.ix, node)
	}
	if err != nil {
		v.err = err.Error()
		v.hits = nil
	} else {
		v.err = ""
		search.Sort(v.ix, v.hits, search.SortName, false)
	}
	v.elapsed = time.Since(start)
	v.selected = 0
	v.top = 0
}

func (v *view) key(ev *tcell.EventKey) (done bool, result string) {
	_, height := v.screen.Size()
	page := max(height-3, 1)
	switch ev.Key() {
	case tcell.KeyEscape, tcell.KeyCtrlC:
		return true, ""
	case tcell.KeyEnter:
		if len(v.hits) == 0 {
			return true, ""
		}
		return true, v.ix.Path(v.hits[v.selected])
	case tcell.KeyCtrlO:
		if len(v.hits) > 0 {
			openWithSystem(v.ix.Path(v.hits[v.selected]))
		}
	case tcell.KeyBackspace, tcell.KeyBackspace2:
		if len(v.input) > 0 {
			v.input = v.input[:len(v.input)-1]
			v.search()
		}
	case tcell.KeyCtrlU:
		v.input = v.input[:0]
		v.search()
	case tcell.KeyCtrlW:
		v.input = deleteWord(v.input)
		v.search()
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
		v.search()
	}
	return false, ""
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
	if v.err != "" {
		status = " " + v.err
	}
	help := "Enter print path  Ctrl-O open  Esc quit "
	statusStyle := tcell.StyleDefault.Reverse(true)
	putString(s, 0, height-1, padRight(status, width-len(help))+help, statusStyle)
	s.Show()
}

func (v *view) drawRow(y, width int, hit uint32, selected bool) {
	e := &v.ix.Entries[hit]
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
	meta := formatMeta(e)
	metaWidth := len(meta) + 1
	if selected {
		putString(v.screen, 0, y, strings.Repeat(" ", width), style)
	}
	x := putString(v.screen, 0, y, " "+e.Name, nameStyle)
	if dir != "" {
		available := width - x - metaWidth - 2
		if available > 4 {
			x = putString(v.screen, x, y, "  "+truncate(dir, available), style.Dim(true))
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
	cmd.Stdout, cmd.Stderr = nil, nil
	_ = cmd.Start()
	go func() { _ = cmd.Wait() }()
}
