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

test: paige-demo
	sh tests/run.sh

fmt:
	clang-format -i src/*.c src/*.h include/*.h

clean:
	rm -rf build paige-demo tests/build

.PHONY: all test fmt clean
