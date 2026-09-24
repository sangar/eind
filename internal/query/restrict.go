package query

// Restrict narrows node to a folder and to files or folders only, the way the
// --path, --files and --dirs command line options do.
func Restrict(node Node, path string, filesOnly, dirsOnly bool) Node {
	kids := []Node{node}
	if path != "" {
		kids = append(kids, InFolder{Path: path})
	}
	if filesOnly {
		kids = append(kids, IsDir{Dir: false})
	}
	if dirsOnly {
		kids = append(kids, IsDir{Dir: true})
	}
	if len(kids) == 1 {
		return node
	}
	return And{Kids: kids}
}
