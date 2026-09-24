VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
LDFLAGS  = -s -w -X main.version=$(VERSION)
TARGETS  = darwin/arm64 darwin/amd64 linux/amd64 linux/arm64 freebsd/amd64 windows/amd64

.PHONY: build test release clean

build:
	go build -ldflags '$(LDFLAGS)' -o eind .

test:
	go vet ./...
	go test ./...

release:
	@for target in $(TARGETS); do \
		os=$${target%/*}; arch=$${target#*/}; ext=; \
		[ $$os = windows ] && ext=.exe; \
		out=dist/eind-$(VERSION)-$$os-$$arch$$ext; \
		echo "  $$out"; \
		GOOS=$$os GOARCH=$$arch CGO_ENABLED=0 go build -ldflags '$(LDFLAGS)' -o $$out . || exit 1; \
	done

clean:
	rm -rf eind dist
