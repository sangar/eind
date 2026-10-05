//go:build !unix

package index

import "os"

// mapFile reads the whole file where mapping is not available. Windows
// cannot rename over a mapped file, which Save relies on.
func mapFile(path string) ([]byte, func() error, error) {
	data, err := os.ReadFile(path)
	return data, func() error { return nil }, err
}
