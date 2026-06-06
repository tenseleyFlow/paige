# paige — portable Makefile (GNU make and BSD make compatible).
# Feature macros: _DEFAULT_SOURCE exposes termios/winsize/O_CLOEXEC on glibc,
# _DARWIN_C_SOURCE the same on macOS; the BSDs expose them by default.

CC ?= cc
WARN = -Wall -Wextra -Wshadow -Wconversion
FEATURE = -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
CFLAGS ?= -std=c11 -O2
ALLCFLAGS = $(CFLAGS) $(FEATURE) $(WARN) -Iinclude

all: paige-demo

build/term.o: src/term.c src/term.h include/paige.h
	@mkdir -p build
	$(CC) $(ALLCFLAGS) -c src/term.c -o build/term.o

build/search.o: src/search.c src/search.h
	@mkdir -p build
	$(CC) $(ALLCFLAGS) -c src/search.c -o build/search.o

build/pager.o: src/pager.c src/term.h src/search.h include/paige.h
	@mkdir -p build
	$(CC) $(ALLCFLAGS) -c src/pager.c -o build/pager.o

build/libpaige.a: build/term.o build/search.o build/pager.o
	ar rcs build/libpaige.a build/term.o build/search.o build/pager.o

build/demo.o: src/demo.c include/paige.h
	@mkdir -p build
	$(CC) $(ALLCFLAGS) -c src/demo.c -o build/demo.o

paige-demo: build/demo.o build/libpaige.a
	$(CC) $(CFLAGS) $(FEATURE) -o paige-demo build/demo.o build/libpaige.a

examples/memory: examples/memory.c include/paige.h build/libpaige.a
	$(CC) $(ALLCFLAGS) -o examples/memory examples/memory.c build/libpaige.a

examples: examples/memory

test: paige-demo
	sh tests/run.sh

bench-smoke: paige-demo
	sh bench/pager.sh --smoke

# Enforcing perf gate: assert paige's deterministic engine counters stay
# O(screen) against bench/baseline.json. Noise-free (counter-based, not
# wall-clock); this is the CI regression gate.
bench-check: paige-demo
	sh bench/check.sh

# Rebuild everything under ASan/UBSan and run the suite (the pty test drives the
# instrumented demo, so the engine is what gets checked). Matches the CI job.
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-std=c11 -O1 -g -fsanitize=address,undefined"
	$(MAKE) test

fmt:
	clang-format -i src/*.c src/*.h include/*.h bench/*.c examples/*.c

clean:
	rm -rf build paige-demo tests/build examples/memory

.PHONY: all examples test bench-smoke bench-check asan fmt clean
