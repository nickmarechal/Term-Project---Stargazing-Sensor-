# term-project (Dark Sky Monitor) — CS 370
# Every target does what its name says. CLAUDE.md treats a lying target as a bug.

CC := gcc

# _POSIX_C_SOURCE: with -std=c17, the Pi's glibc hides fsync, ftruncate, pread
# and clock_gettime unless this is defined, and -Werror then fails the build.
# macOS shows them anyway, so leaving this out would only break on the Pi.
CFLAGS := -std=c17 -Wall -Wextra -Werror -g -O1 -Isrc -Itests \
          -D_POSIX_C_SOURCE=200809L
# -fno-sanitize-recover: undefined behavior stops the run instead of printing
# a message and carrying on, so `make asan` fails on it like it does on ASan.
SANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

# Portable library code (no main), built and tested on macOS and Linux.
# Each side adds files only to its own section (docs/m3-plan.md).

# --- shared (src/common) ---
COMMON_SRC  := src/common/crc32.c src/common/record.c src/common/log.c
COMMON_TSRC := tests/test_crc32.c tests/test_record.c tests/test_log.c
COMMON_HDR  := src/common/crc32.h src/common/record.h src/common/log.h

# --- storage side (src/store, src/dsq) ---
STORE_SRC  := src/store/store.c
STORE_TSRC := tests/test_store.c tests/test_dsq.c
STORE_HDR  := src/store/store.h src/dsq/dump.h
DSQ_LIB    := src/dsq/dump.c
DSQ_SRC    := src/dsq/dsq.c
# Linux only (SOCK_SEQPACKET). Sensor programs link STORE_CLIENT_SRC.
STORE_CLIENT_SRC := src/store/store_client.c
STORE_CLIENT_HDR := src/store/store_client.h
STORED_SRC       := src/store/stored.c
STORE_ITSRC      := tests/itest_stored.c

# --- sensor side (src/sensors; portable parts only) ---
SENSOR_SRC  := src/sensors/convert.c
SENSOR_TSRC := tests/test_convert.c
SENSOR_HDR  := src/sensors/convert.h

SRC  := $(COMMON_SRC) $(STORE_SRC) $(DSQ_LIB) $(SENSOR_SRC)
TSRC := tests/test_main.c $(COMMON_TSRC) $(STORE_TSRC) $(SENSOR_TSRC)
HDR  := tests/check.h tests/suites.h $(COMMON_HDR) $(STORE_HDR) $(SENSOR_HDR)

TESTBIN := build/test_all

# Programs that need Linux: built, run under asan and valgrind only there.
UNAME := $(shell uname -s)
ifeq ($(UNAME),Linux)
LINUX_BIN  := build/stored build/itest_stored
LINUX_TEST := itest
endif

## all: build the test binary and dsq (and stored + its integration test on Linux)
all: $(TESTBIN) build/dsq $(LINUX_BIN)

$(TESTBIN): $(SRC) $(TSRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(TSRC) -o $@

build/dsq: $(DSQ_SRC) $(DSQ_LIB) $(COMMON_SRC) $(STORE_SRC) $(COMMON_HDR) $(STORE_HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(DSQ_SRC) $(DSQ_LIB) $(COMMON_SRC) $(STORE_SRC) -o $@

build/stored: $(STORED_SRC) $(COMMON_SRC) $(STORE_SRC) $(COMMON_HDR) $(STORE_HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(STORED_SRC) $(COMMON_SRC) $(STORE_SRC) -o $@

build/itest_stored: $(STORE_ITSRC) $(STORE_CLIENT_SRC) $(COMMON_SRC) $(COMMON_HDR) \
                    $(STORE_HDR) $(STORE_CLIENT_HDR) tests/check.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(STORE_ITSRC) $(STORE_CLIENT_SRC) $(COMMON_SRC) -o $@

## test: run every test suite
test: $(TESTBIN)
	./$(TESTBIN)

## itest: run the real stored and talk to it over its socket (Linux only)
ifeq ($(UNAME),Linux)
itest: build/stored build/itest_stored
	./build/itest_stored ./build/stored
else
itest:
	@echo "make itest: needs Linux (macOS has no SOCK_SEQPACKET) - run this on the Pi."
endif

## asan: rebuild with AddressSanitizer + UBSan and run the tests (and itest on Linux)
asan: CFLAGS += $(SANFLAGS)
asan: clean test $(LINUX_TEST)

## memcheck: run the tests (and itest, with stored traced too) under valgrind (Linux/Pi only)
memcheck: all
	@if ! command -v valgrind >/dev/null 2>&1; then \
	    echo "make memcheck: valgrind not installed here (expected on macOS - run this on the Pi)."; \
	  else \
	    valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TESTBIN) && \
	    if [ -n "$(LINUX_BIN)" ]; then \
	      valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 \
	        --trace-children=yes --child-silent-after-fork=yes \
	        ./build/itest_stored ./build/stored; \
	    fi; \
	  fi

## soakcheck: 1-hour miniature of the graded 48h soak
soakcheck:
	@echo "make soakcheck: not implemented yet - needs the supervisor and heartbeat logging."
	@echo "Per spec 3.4 this must exercise: startup, hourly heartbeat (liveness/RSS/event counts),"
	@echo "an injected fault with detect-degrade-recover, and an orderly shutdown."

## deploy: rsync the tree to the Pi. usage: make deploy PI=pi@hostname
deploy:
	@if [ -z "$(PI)" ]; then \
	    echo "usage: make deploy PI=pi@hostname"; exit 1; \
	  else \
	    rsync -av --exclude '.git' --exclude 'build' --exclude 'transcripts' ./ $(PI):~/term-project/; \
	  fi

## clean: remove build artifacts
clean:
	rm -rf build

## help: list targets
help:
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/^## /  /'

.PHONY: all test itest asan memcheck soakcheck deploy clean help
