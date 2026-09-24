//go:build darwin || freebsd || netbsd

package index

import (
	"os"
	"syscall"
)

func birthTime(info os.FileInfo) int64 {
	if st, ok := info.Sys().(*syscall.Stat_t); ok {
		return st.Birthtimespec.Sec
	}
	return 0
}
