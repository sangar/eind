VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
LDFLAGS  = -s -w -X main.version=$(VERSION)
GORELEASER = go run github.com/goreleaser/goreleaser/v2@latest
STATICCHECK = go run honnef.co/go/tools/cmd/staticcheck@latest

.PHONY: build test snapshot release clean

# The watcher needs cgo for FSEvents on macOS; elsewhere the binary is static.
CGO = $(if $(filter Darwin,$(shell uname -s)),1,0)

build:
	CGO_ENABLED=$(CGO) go build -ldflags '$(LDFLAGS)' -o eind .

test:
	go vet ./...
	$(STATICCHECK) ./...
	go test -race ./...

snapshot:
	$(GORELEASER) release --snapshot --clean

release:
	$(GORELEASER) release --clean

clean:
	rm -rf eind dist
