# term-project (Dark Sky Monitor) — CS 370
# Every target does what its name says. CLAUDE.md treats a lying target as a bug.

CC := gcc

# _POSIX_C_SOURCE: with -std=c17, the Pi's glibc hides fsync, ftruncate, pread
# and clock_gettime unless this is defined, and -Werror then fails the build.
# macOS shows them anyway, so leaving this out would only break on the Pi.
CFLAGS := -std=c17 -Wall -Wextra -Werror -g -O1 -Isrc -Itests \
          -D_POSIX_C_SOURCE=200809L
SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

# Portable library code (no main), built and tested on macOS and Linux.
# Each side adds files only to its own section (docs/m3-plan.md).

# --- shared (src/common) ---
COMMON_SRC  := src/common/crc32.c src/common/record.c src/common/log.c
COMMON_TSRC := tests/test_crc32.c tests/test_record.c tests/test_log.c
COMMON_HDR  := src/common/crc32.h src/common/record.h src/common/log.h

# --- storage side (src/store, src/dsq) ---
STORE_SRC  := src/store/store.c
STORE_TSRC := tests/test_store.c
STORE_HDR  := src/store/store.h

# --- sensor side (src/sensors; portable parts only) ---
SENSOR_SRC  :=
SENSOR_TSRC :=
SENSOR_HDR  :=

SRC  := $(COMMON_SRC) $(STORE_SRC) $(SENSOR_SRC)
TSRC := tests/test_main.c $(COMMON_TSRC) $(STORE_TSRC) $(SENSOR_TSRC)
HDR  := tests/check.h tests/suites.h $(COMMON_HDR) $(STORE_HDR) $(SENSOR_HDR)

TESTBIN := build/test_all

## all: build the test binary
all: $(TESTBIN)

$(TESTBIN): $(SRC) $(TSRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(TSRC) -o $@

## test: run every test suite
test: $(TESTBIN)
	./$(TESTBIN)

## asan: rebuild with AddressSanitizer + UBSan and run the tests
asan: CFLAGS += $(SANFLAGS)
asan: clean test

## memcheck: run the tests under valgrind (Linux/Pi only, not on macOS)
memcheck: all
	@if ! command -v valgrind >/dev/null 2>&1; then \
	    echo "make memcheck: valgrind not installed here (expected on macOS - run this on the Pi)."; \
	  else \
	    valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./$(TESTBIN); \
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

.PHONY: all test asan memcheck soakcheck deploy clean help
