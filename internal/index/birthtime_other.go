//go:build !darwin && !freebsd && !netbsd && !windows

package index

import "os"

// Linux only exposes birth time through statx, which os.FileInfo does not
// carry, so creation dates are unavailable there.
func birthTime(os.FileInfo) int64 { return 0 }
