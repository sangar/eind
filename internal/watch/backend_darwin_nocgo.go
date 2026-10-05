//go:build darwin && !cgo

package watch

import "errors"

// Without cgo, notify watches macOS with kqueue, which needs an open file for
// every file below the roots and never gets through a home directory.
var errUnsupported = errors.New("this eind was built without cgo, so it cannot use FSEvents to watch macOS; build it with CGO_ENABLED=1")
