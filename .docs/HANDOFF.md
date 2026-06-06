# paige — handoff & roadmap

This document is the pickup point for anyone (human or agent) continuing paige as
an **independent** terminal-pager library. It assumes no prior context beyond the
source. It is tracked so it ships with the repo (unlike the cloned reference
source under `.docs/refs/`).

paige is embedded as a git submodule inside the `mat` project (`mat/lib/paige`),
but it stands alone: zero dependencies, a stable C API, its own build and CI. The
goal is a small, **fast**, dependency-free pager engine that a host program
drives by supplying a per-line render callback.

---

## 1. What paige is

A pager *engine*, not a pager program. The host owns the content; paige owns the
terminal. The contract:

- The host implements `render_line(ctx, lineno, width, sink)` (or the extended
  `render_line_ex`) — emit the visual segments of one logical line for `width`
  columns via `paige_emit()`; return the segment count, or `0` at EOF.
- paige calls it **only for the lines it needs to show**. Paging a multi-gigabyte
  file never renders more than a screenful — the lazy contract is the whole point
  and the source of paige's speed advantage over buffer-first pagers.
- Keys are read from `/dev/tty`, not stdin, so the **content may be piped in**
  while the keyboard still drives the pager.

`paige_run(doc, opts)` returns `0` on a clean quit, `-1` if there is no usable
controlling terminal (the host should then print plainly).

## 2. Architecture (one secret per file)

| file | responsibility |
|---|---|
| `include/paige.h` | the entire public API: `paige_doc`, `paige_opts`, `paige_render_req`, `paige_stats`, `paige_emit`, `paige_run`. Keep this surface minimal and append-only. |
| `src/term.h` / `term.c` | terminal control: raw mode (ISIG/IXON cleared), alternate screen, size, SIGWINCH, fatal-signal restore, key decoding. |
| `src/search.c` / `search.h` | the literal smart-case matcher (`memchr` first-byte skip), used by the engine's search. |
| `src/pager.c` | the engine: `seglist`, `view` (scroll position as logical line `L` + visual segment `S`), `outbuf`, scroll math, windowed `draw()`, the key loop, digit-goto, search, marks, follow, help/perf overlays. |
| `src/demo.c` | a reference client — a standalone byte-wrapping pager that honors the segment window. Real clients (e.g. mat) supply their own width-aware renderer. |
| `tests/pty_test.c` | drives `paige-demo` inside a real pty via `forkpty`, feeds keystrokes, asserts on the rendered screen and stat counters. |
| `bench/` | `pager.sh` + `pty_driver.c` benchmark harness; `BASELINE.md` records numbers. |

`view` tracks the top of the screen as a (logical line, visual segment) pair,
because one logical line may wrap to several visual rows. `draw()` renders each
line **once per frame** and requests only the visible segment window
(`paige_render_req.seg_first`/`seg_max`), so a long wrapped line stays O(visible);
a renderer that ignores the window (emits everything) still works — the engine
detects the full emission (`sl->n == total`) and indexes from `S`.

## 3. Current state — what works

- **Navigation**: `j`/`k`/`↑`/`↓`, space/`f`/`b` (page), `d`/`u` (half-page),
  `g`/`G` (top/bottom, lazy — `G` is O(screen) via `line_count`), `q`/`Ctrl-C`.
- **Search**: `/` `?` `n` `N`, incremental preview (bounded per keystroke, never
  wraps), authoritative full search on Enter (wrap-around, smart-case, "search
  wrapped" / "pattern not found"), match highlighting, keypress-cancel of a long
  scan, a "match n of N" overview, progressive "searching…" status, and `&` to
  filter the view to matching lines (reversibly).
- **Horizontal scroll / chop mode** (`←`/`→`, `chop_long_lines`): `<`/`>`
  overflow markers, search-hit reveal, a `col a-b/len` readout, and `|` for a
  column ruler.
- **Semantic jumps** (`]`/`[`): jump to host-defined landmarks via the
  `landmark` hook (the demo recognizes `#` headers and ERROR/WARN lines).
- **Multiple documents** (`:n`/`:p`): `paige_run_many()` pages an ordered set.
- **Niceties**: percent goto (`50%`), marks (`m`/`'`, plus `''`/`'^`/`'$`/`'.`),
  `h` help overlay, `P` performance panel, transient status messages, live
  incremental goto-line.
- **Follow mode** (`F`, tail-f style) via the host `refresh` hook; pauses on
  manual nav, and highlights freshly-appended lines (`paige_render_req.appended`).
- **Terminal safety**: ISIG/IXON cleared so the pager owns Ctrl-C/Ctrl-S;
  SA_RESETHAND handlers restore the terminal on any fatal signal.
- `quit_if_one_screen`: print plainly and return when the content fits.
- Tested via the pty harness on Linux, macOS, FreeBSD (CI). ASan/UBSan clean,
  builds `-Werror` under GNU make and bmake.

## 4. Build / test

```sh
make            # builds ./paige-demo + build/libpaige.a
make test       # runs tests/run.sh (unit + the pty integration test)
make asan       # rebuild under ASan/UBSan and run the suite
make bench-smoke   # bench/pager.sh --smoke
make examples   # examples/memory
make fmt        # clang-format -i (LLVM base, 19.1.7 in CI)
```

CI (`.github/workflows/ci.yml`): lint (clang-format **19.1.7**), build matrix
`{ubuntu,macos}×{gcc,clang}` with the strict warning set, a FreeBSD VM job, a
sanitizers job, and a benchmark smoke job; `all-jobs` is the aggregate gate. The
test steps set `PAIGE_TEST_STRICT=1` so a missing pty fails CI rather than
skipping. Match clang-format 19.1.7 exactly — minor version skew reflows
differently.

## 5. Roadmap — what's left

The pager features, the Sprint-7 differentiators (search overview, progressive
status, semantic jumps via the `landmark` hook, long-line ruler/readout,
reversible filters), and the Sprint-5 deferrals (multidoc `:n`/`:p`,
appended-line highlight) are all **done**. What's left is smaller polish:

- **Minimap / collapse**: the search overview shows "n of N" but not a visual
  minimap; long-line tools have a ruler/readout but no fold/collapse.
- **Allocation introspection**: `segments_emitted` exists; per-frame
  terminal-damage and allocation counters could extend `paige_stats`.
- **Mouse wheel** is still host-owned/deferred.

`paige_emit`/`ob_put` degrade gracefully on `realloc` failure (drop the segment
rather than crash); a future hardening pass could surface an error flag.

## 6. Timing & the pty test

The pty harness (`tests/pty_test.c` + `pty_helpers.h`) waits for the *expected
token* after each keystroke (`pty_wait_for`), not a fixed idle window, so a slow
or cold box just takes longer instead of racing the read. It **skips** (exit 0)
when there is no pty, unless `PAIGE_TEST_STRICT=1` (CI). It honors `$TMPDIR` and
self-times-out via `alarm(60)` as a hang backstop.

Digit-goto gotcha: digit entry holds a timed read for `goto_pause_ms`
(`PAIGE_GOTO_MS`, default 600ms). The pause-commit tests sleep past the timeout
deliberately; that wall-clock stimulus is intentional, not a read race. FreeBSD
needs `<sys/ioctl.h>`/`<termios.h>` before `<libutil.h>` (for `struct winsize`).

## 7. Performance

`bench/pager.sh` + `bench/pty_driver.c` benchmark against `less`/`ov`/`moar` on a
**real-source corpus** (this repo tiled to size — never `/dev/zero` or
fixed-width random). `BASELINE.md` records numbers. The driver reports paige's
`render`/`segments`/`search_lines` stat counters, which **isolate engine work**
from `first_ms` (end-to-end TTFB that includes the demo host's eager
slurp+index). The honest result: engine render/segments stay O(screen)
regardless of file size, a huge wrapped line is O(visible), and `less` wins
first-paint because the *demo host* indexes eagerly (not an engine limitation).

## 8. Reference implementations

Cloned under `.docs/refs/` (gitignored): `less`, `ov`, `moar`, `pspg`. Study
less's search state machine and line-position model; ov/moar's incremental search
UX; pspg for structured/column content (relevant to the long-line focus tools).

## 9. Conventions

- C11 + POSIX. Portable across Linux, macOS, FreeBSD; build under GNU make and
  bmake. Guard platform headers (`pty.h` Linux / `util.h` macOS / `libutil.h`
  BSD).
- Warnings are errors in CI (`-Wconversion` included — mind implicit narrowing).
- clang-format (LLVM base, `IndentWidth: 4`, `ColumnLimit: 80`); avoid multi-line
  ternaries and trailing comments that push past 80.
- Keep `include/paige.h` tiny and append-only; new behavior is opt-in through
  `paige_opts`/`paige_render_req`, defaulting to current behavior on a zeroed
  struct.
- Commits: terse, imperative, no co-author/generated trailers.

## 10. Release checklist

1. `make fmt` clean; `make CFLAGS="-std=c11 -O2 -Werror"` and `make asan` green
   under both `make` and `bmake`.
2. `make test` green locally and across the CI matrix (incl. FreeBSD VM).
3. Bump `PAIGE_VERSION_{MAJOR,MINOR,PATCH}` in `include/paige.h`; verify
   `paige-demo --version` reports it.
4. Re-record `bench/BASELINE.md` (`sh bench/pager.sh --size-mb 64`) if perf-
   relevant code changed.
5. Update §3/§5 here and `README.md` if public behavior changed.
6. Tag; if vendored in mat, bump the submodule gitlink and the AUR `#commit=`.
