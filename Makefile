VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c23 -D_DEFAULT_SOURCE -D_GNU_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror -Wshadow -Wconversion -Wvla -Wstrict-prototypes \
           -Wimplicit-fallthrough -Wno-unused-parameter \
           -pthread -DEIND_VERSION='"$(VERSION)"'
LDLIBS  += -pthread
PREFIX  ?= $(HOME)/.local
BINDIR   = $(PREFIX)/bin

ifeq ($(shell uname -s),Darwin)
LDLIBS  += -framework CoreServices
endif

LIB_SRC  = $(wildcard src/core/*.c src/index/*.c src/fs/*.c src/app/*.c)
LIB_OBJ  = $(LIB_SRC:src/%.c=build/%.o)
TEST_OBJ = build/tests/test_eind.o

.PHONY: all test sanitize clean install uninstall

all: eind

eind: $(LIB_OBJ) build/main.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/test_eind: $(LIB_OBJ) $(TEST_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c $(wildcard src/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

build/tests/%.o: tests/%.c $(wildcard src/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

test: build/test_eind
	./build/test_eind

# Rebuilds everything with AddressSanitizer and UndefinedBehaviorSanitizer and runs the tests.
sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"
	$(MAKE) clean

clean:
	rm -rf build eind

# Installs the binary and runs `eind serve` at login. Run again to upgrade: it restarts the service on the new binary.
install: eind
	install -d $(BINDIR)
	install -m 755 eind $(BINDIR)/eind
	$(BINDIR)/eind service enable

# Stops the login service and removes the binary; the index and config are kept.
uninstall:
	if [ -x $(BINDIR)/eind ] && $(BINDIR)/eind status | grep -q '^service: enabled'; then $(BINDIR)/eind service disable; fi
	rm -f $(BINDIR)/eind
