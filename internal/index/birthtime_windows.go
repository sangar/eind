//go:build windows

package index

import (
	"os"
	"syscall"
)

func birthTime(info os.FileInfo) int64 {
	if st, ok := info.Sys().(*syscall.Win32FileAttributeData); ok {
		return st.CreationTime.Nanoseconds() / 1e9
	}
	return 0
}
