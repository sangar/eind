// eind is an instant file search for the command line.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"golang.org/x/term"

	"eind/internal/config"
	"eind/internal/index"
	"eind/internal/output"
	"eind/internal/query"
	"eind/internal/search"
	"eind/internal/tui"
	"eind/internal/watch"
)

var version = "dev"

const usageText = `eind - instant file search for the terminal

Usage:
  eind [options] [query...]     search; with no query, open the interactive view
  eind index [--root DIR]...    build the index from the configured roots
  eind watch                    keep the index up to date from filesystem events
  eind status                   show where the index and config live, and their size
  eind config [--init]          show the effective config, or write a default file
  eind tui                      open the interactive view

Search options:
  -r, --regex          treat terms as regular expressions
  -i, --case           match case
  -w, --whole-word     match whole words only
  -p, --match-path     match against the full path instead of the name
  -n, --max-results N  print at most N results
  -o, --offset N       skip the first N results
  -s, --sort KEY       path (default), name, size, dm, dc or ext
  -d, --descending     reverse the sort order
      --path DIR       only results inside DIR
      --files          only files
      --dirs           only folders

Output options:
      --json           JSON array          --csv          CSV with header
  -0, --null           NUL-separated       --name-only    print names, not paths
      --size           add a size column   --dm, --dc     add date columns
      --count          print only the number of results
      --color WHEN     auto (default), always or never

Global options:
      --config FILE    config file  (default %s, env EIND_CONFIG)
      --index FILE     index file   (default %s, env EIND_INDEX)
  -h, --help           show this help
      --version        show the version

Query syntax:
  space = AND    |  = OR      ! = NOT     <a b|c> = grouping   "quoted words"
  *.txt          wildcards match the whole name
  ext:pdf;docx   size:>10mb   size:1mb..5mb   size:large    dm:today   dm:2024-03
  dm:lastweek    dm:last30days   dc:2024   len:>40   depth:3   file:   folder:
  case:Readme    regex:^draft_\d+   ww:log   wfn:Makefile   path:src/main
  parent:~/Documents   infolder:~/Projects
`

func main() {
	if err := run(os.Args[1:]); err != nil {
		if errors.Is(err, syscall.EPIPE) {
			os.Exit(0)
		}
		fmt.Fprintln(os.Stderr, "eind:", err)
		os.Exit(1)
	}
}

type globals struct {
	configPath string
	indexPath  string
}

func (g *globals) bind(fs *flag.FlagSet) {
	fs.StringVar(&g.configPath, "config", config.ConfigPath(), "")
	fs.StringVar(&g.indexPath, "index", config.IndexPath(), "")
}

func run(args []string) error {
	positional, flags, literal := splitFlags(args, knownValueFlags)
	if len(positional) > 0 && !literal {
		switch positional[0] {
		case "index":
			return cmdIndex(append(flags, positional[1:]...))
		case "watch":
			return cmdWatch(append(flags, positional[1:]...))
		case "status":
			return cmdStatus(flags)
		case "config":
			return cmdConfig(append(flags, positional[1:]...))
		case "tui":
			return cmdSearch(flags, nil, true)
		case "search":
			return cmdSearch(flags, positional[1:], false)
		case "help":
			printUsage(os.Stdout)
			return nil
		}
	}
	return cmdSearch(flags, positional, false)
}

// Flags that take a value, so the pre-pass knows to keep the next argument with them.
var knownValueFlags = map[string]bool{
	"n": true, "max-results": true, "o": true, "offset": true, "s": true, "sort": true,
	"path": true, "color": true, "config": true, "index": true, "root": true, "save-interval": true,
}

// splitFlags lets options appear anywhere on the line,
// while the standard flag package only reads them before the first word.
// Everything after "--" is query text; literal reports that the first word
// came after "--", so it is never taken as a subcommand.
func splitFlags(args []string, takesValue map[string]bool) (positional, flags []string, literal bool) {
	for i := 0; i < len(args); i++ {
		a := args[i]
		if a == "--" {
			literal = len(positional) == 0
			positional = append(positional, args[i+1:]...)
			break
		}
		if len(a) < 2 || a[0] != '-' || strings.HasPrefix(a, "-.") || (isNumber(a[1:]) && a != "-0") {
			positional = append(positional, a)
			continue
		}
		flags = append(flags, a)
		name := strings.TrimLeft(a, "-")
		if !strings.Contains(name, "=") && takesValue[name] && i+1 < len(args) {
			i++
			flags = append(flags, args[i])
		}
	}
	return positional, flags, literal
}

func isNumber(s string) bool {
	for _, c := range s {
		if c < '0' || c > '9' {
			return false
		}
	}
	return s != ""
}

func newFlagSet(name string) *flag.FlagSet {
	fs := flag.NewFlagSet(name, flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	fs.Usage = func() {}
	return fs
}

func printUsage(w io.Writer) {
	fmt.Fprintf(w, usageText, config.ConfigPath(), config.IndexPath())
}

func parseFlags(fs *flag.FlagSet, args []string) error {
	err := fs.Parse(args)
	if errors.Is(err, flag.ErrHelp) {
		printUsage(os.Stdout)
		os.Exit(0)
	}
	if err != nil {
		return fmt.Errorf("%w (see eind --help)", err)
	}
	return nil
}

type searchFlags struct {
	regex, caseSensitive, wholeWord, matchPath bool
	maxResults, offset                         int
	sortKey                                    string
	descending                                 bool
	path                                       string
	filesOnly, dirsOnly                        bool
	json, csv, null, nameOnly                  bool
	showSize, showModified, showCreated        bool
	count                                      bool
	color                                      string
	showVersion                                bool
}

func (f *searchFlags) bind(fs *flag.FlagSet) {
	for _, name := range []string{"r", "regex"} {
		fs.BoolVar(&f.regex, name, false, "")
	}
	for _, name := range []string{"i", "case"} {
		fs.BoolVar(&f.caseSensitive, name, false, "")
	}
	for _, name := range []string{"w", "whole-word"} {
		fs.BoolVar(&f.wholeWord, name, false, "")
	}
	for _, name := range []string{"p", "match-path"} {
		fs.BoolVar(&f.matchPath, name, false, "")
	}
	for _, name := range []string{"n", "max-results"} {
		fs.IntVar(&f.maxResults, name, 0, "")
	}
	for _, name := range []string{"o", "offset"} {
		fs.IntVar(&f.offset, name, 0, "")
	}
	for _, name := range []string{"s", "sort"} {
		fs.StringVar(&f.sortKey, name, "path", "")
	}
	for _, name := range []string{"d", "descending"} {
		fs.BoolVar(&f.descending, name, false, "")
	}
	for _, name := range []string{"0", "null"} {
		fs.BoolVar(&f.null, name, false, "")
	}
	fs.StringVar(&f.path, "path", "", "")
	fs.BoolVar(&f.filesOnly, "files", false, "")
	fs.BoolVar(&f.dirsOnly, "dirs", false, "")
	fs.BoolVar(&f.json, "json", false, "")
	fs.BoolVar(&f.csv, "csv", false, "")
	fs.BoolVar(&f.nameOnly, "name-only", false, "")
	fs.BoolVar(&f.showSize, "size", false, "")
	fs.BoolVar(&f.showModified, "dm", false, "")
	fs.BoolVar(&f.showCreated, "dc", false, "")
	fs.BoolVar(&f.count, "count", false, "")
	fs.StringVar(&f.color, "color", "auto", "")
	fs.BoolVar(&f.showVersion, "version", false, "")
}

func cmdSearch(flagArgs, terms []string, forceTUI bool) error {
	fs := newFlagSet("eind")
	var g globals
	var f searchFlags
	g.bind(fs)
	f.bind(fs)
	if err := parseFlags(fs, flagArgs); err != nil {
		return err
	}
	if f.showVersion {
		fmt.Println("eind", version)
		return nil
	}
	sortKey, err := search.ParseSortKey(f.sortKey)
	if err != nil {
		return err
	}
	ix, err := loadOrBuild(g)
	if err != nil {
		return err
	}
	defaults := query.Defaults{Regex: f.regex, CaseSensitive: f.caseSensitive, WholeWord: f.wholeWord, MatchPath: f.matchPath}

	if forceTUI || (len(terms) == 0 && stdoutIsTerminal() && !f.count && !f.json && !f.csv) {
		chosen, err := tui.Run(ix, defaults)
		if err != nil {
			return err
		}
		if chosen != "" {
			fmt.Println(chosen)
		}
		return nil
	}

	node, err := query.Parse(strings.Join(terms, " "), defaults)
	if err != nil {
		return err
	}
	node = applyFilters(node, f)
	hits, err := search.Run(ix, node)
	if err != nil {
		return err
	}
	if f.count {
		fmt.Println(len(hits))
		return nil
	}
	search.Sort(ix, hits, sortKey, f.descending)
	hits = hits[min(f.offset, len(hits)):]
	if f.maxResults > 0 && f.maxResults < len(hits) {
		hits = hits[:f.maxResults]
	}
	opts := output.Options{
		NullSep: f.null, NameOnly: f.nameOnly,
		ShowSize: f.showSize, ShowModified: f.showModified, ShowCreated: f.showCreated,
		Color: useColor(f.color),
	}
	switch {
	case f.json:
		opts.Format = output.JSON
	case f.csv:
		opts.Format = output.CSV
	}
	return output.Write(os.Stdout, ix, hits, opts)
}

func applyFilters(node query.Node, f searchFlags) query.Node {
	kids := []query.Node{node}
	if f.path != "" {
		kids = append(kids, query.InFolder{Path: f.path})
	}
	if f.filesOnly {
		kids = append(kids, query.IsDir{Dir: false})
	}
	if f.dirsOnly {
		kids = append(kids, query.IsDir{Dir: true})
	}
	if len(kids) == 1 {
		return node
	}
	return query.And{Kids: kids}
}

func useColor(mode string) bool {
	switch mode {
	case "always":
		return true
	case "never":
		return false
	}
	return stdoutIsTerminal() && os.Getenv("NO_COLOR") == ""
}

func stdoutIsTerminal() bool { return term.IsTerminal(int(os.Stdout.Fd())) }
func stderrIsTerminal() bool { return term.IsTerminal(int(os.Stderr.Fd())) }

func loadOrBuild(g globals) (*index.Index, error) {
	ix, err := index.Load(g.indexPath)
	if errors.Is(err, index.ErrNotFound) {
		fmt.Fprintf(os.Stderr, "No index at %s yet; building one (run `eind index` to rebuild later).\n", g.indexPath)
		cfg, err := config.Load(g.configPath)
		if err != nil {
			return nil, err
		}
		return buildIndex(cfg, g.indexPath)
	}
	return ix, err
}

func buildIndex(cfg config.Config, indexPath string) (*index.Index, error) {
	ex, err := index.NewExcludes(cfg.Excludes)
	if err != nil {
		return nil, err
	}
	start := time.Now()
	ix := index.New(nil)
	errorsSeen := 0
	progress := func(int) {}
	if stderrIsTerminal() {
		progress = func(added int) { fmt.Fprintf(os.Stderr, "\r  %s entries...", commas(len(ix.Entries))) }
	}
	for _, root := range cfg.Roots {
		abs, err := filepath.Abs(root)
		if err != nil {
			return nil, err
		}
		res, err := ix.AddTree(abs, index.NoParent, ex, progress)
		if err != nil {
			fmt.Fprintf(os.Stderr, "skipping %s: %v\n", abs, err)
			continue
		}
		ix.Roots = append(ix.Roots, abs)
		errorsSeen += res.Errors
	}
	if len(ix.Roots) == 0 {
		return nil, errors.New("none of the configured roots could be read")
	}
	if err := ix.Save(indexPath); err != nil {
		return nil, err
	}
	files, dirs := ix.Stats()
	if stderrIsTerminal() {
		fmt.Fprint(os.Stderr, "\r\x1b[K")
	}
	fmt.Fprintf(os.Stderr, "Indexed %s files and %s folders in %s", commas(files), commas(dirs), time.Since(start).Round(10*time.Millisecond))
	if errorsSeen > 0 {
		fmt.Fprintf(os.Stderr, " (%d folders unreadable)", errorsSeen)
	}
	if st, err := os.Stat(indexPath); err == nil {
		fmt.Fprintf(os.Stderr, "; index is %s at %s", output.HumanSize(st.Size()), indexPath)
	}
	fmt.Fprintln(os.Stderr)
	return ix, nil
}

func cmdIndex(args []string) error {
	fs := newFlagSet("eind index")
	var g globals
	g.bind(fs)
	var roots multiFlag
	fs.Var(&roots, "root", "")
	if err := parseFlags(fs, args); err != nil {
		return err
	}
	cfg, err := config.Load(g.configPath)
	if err != nil {
		return err
	}
	if len(roots) > 0 {
		cfg.Roots = roots
	}
	_, err = buildIndex(cfg, g.indexPath)
	return err
}

type multiFlag []string

func (m *multiFlag) String() string     { return strings.Join(*m, ",") }
func (m *multiFlag) Set(v string) error { *m = append(*m, v); return nil }

func cmdWatch(args []string) error {
	fs := newFlagSet("eind watch")
	var g globals
	g.bind(fs)
	interval := fs.Duration("save-interval", 10*time.Second, "")
	if err := parseFlags(fs, args); err != nil {
		return err
	}
	cfg, err := config.Load(g.configPath)
	if err != nil {
		return err
	}
	ex, err := index.NewExcludes(cfg.Excludes)
	if err != nil {
		return err
	}
	ix, err := loadOrBuild(g)
	if err != nil {
		return err
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	fmt.Fprintln(os.Stderr, watch.ServiceHint())
	return watch.Run(ctx, ix, ex, g.indexPath, *interval, os.Stderr)
}

func cmdStatus(args []string) error {
	fs := newFlagSet("eind status")
	var g globals
	g.bind(fs)
	if err := parseFlags(fs, args); err != nil {
		return err
	}
	fmt.Printf("config: %s", g.configPath)
	if _, err := os.Stat(g.configPath); err != nil {
		fmt.Print(" (not present, using defaults)")
	}
	fmt.Println()
	fmt.Printf("index:  %s", g.indexPath)
	st, err := os.Stat(g.indexPath)
	if err != nil {
		fmt.Println(" (not built yet; run `eind index`)")
		return nil
	}
	fmt.Printf(" (%s)\n", output.HumanSize(st.Size()))
	start := time.Now()
	ix, err := index.Load(g.indexPath)
	if err != nil {
		return err
	}
	files, dirs := ix.Stats()
	fmt.Printf("built:  %s\n", ix.BuiltAt.Format("2006-01-02 15:04:05"))
	fmt.Printf("loaded: %s files, %s folders in %s\n", commas(files), commas(dirs), time.Since(start).Round(time.Millisecond))
	for _, r := range ix.Roots {
		fmt.Printf("root:   %s\n", r)
	}
	return nil
}

func cmdConfig(args []string) error {
	fs := newFlagSet("eind config")
	var g globals
	g.bind(fs)
	initialize := fs.Bool("init", false, "")
	if err := parseFlags(fs, args); err != nil {
		return err
	}
	if *initialize {
		created, err := config.WriteDefault(g.configPath)
		if err != nil {
			return err
		}
		if created {
			fmt.Println("wrote", g.configPath)
		} else {
			fmt.Println(g.configPath, "already exists")
		}
		return nil
	}
	cfg, err := config.Load(g.configPath)
	if err != nil {
		return err
	}
	fmt.Printf("# %s\n", g.configPath)
	fmt.Print(config.Render(cfg))
	return nil
}

func commas(n int) string {
	s := fmt.Sprint(n)
	for i := len(s) - 3; i > 0; i -= 3 {
		s = s[:i] + "," + s[i:]
	}
	return s
}
