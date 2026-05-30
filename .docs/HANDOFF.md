# paige — handoff & roadmap

This document is the pickup point for anyone (human or agent) continuing paige as
an **independent** terminal-pager library. It assumes no prior context beyond the
source. Unlike cloned reference source (`.docs/refs/`, gitignored), this file is
tracked so it ships with the repo.

paige is currently embedded as a git submodule inside the `mat` project
(`mat/lib/paige`), but it is designed to stand alone: zero dependencies, a stable
C API, its own build and CI. The goal is a small, **fast**, dependency-free pager
engine that a host program drives by supplying a per-line render callback.

---

## 1. What paige is

A pager *engine*, not a pager program. The host owns the content; paige owns the
terminal. The contract:

- The host implements `int render_line(ctx, lineno, width, sink)` — emit the
  visual segments of one logical line, laid out for `width` columns, by calling
  `paige_emit()` once per segment; return the segment count, or `0` at EOF.
- paige calls it **only for the lines it needs to show**. Paging a multi-gigabyte
  file never renders more than a screenful — this lazy contract is the whole
  point and the source of paige's speed advantage over buffer-first pagers.
- Keys are read from `/dev/tty`, not stdin, so the **content may be piped in**
  while the keyboard still drives the pager.

`paige_run(doc, opts)` returns `0` on a clean quit, `-1` if there is no usable
controlling terminal (the host should then print plainly).

## 2. Architecture (one secret per file)

| file | responsibility |
|---|---|
| `include/paige.h` | the entire public API: `paige_doc`, `paige_opts`, `paige_emit`, `paige_run`. Keep this surface minimal and stable. |
| `src/term.h` / `term.c` | terminal control: raw mode, alternate screen, size, SIGWINCH, key decoding (`paige_term_key`, `paige_term_key_timed`). All POSIX termios + ANSI. |
| `src/pager.c` | the engine: `seglist` (visual segments of one line), `view` (scroll position as logical line `L` + visual segment `S`), `outbuf` (one-write screen flush), scroll math, `draw()`, the key loop, digit-goto. |
| `src/demo.c` | a reference client — a standalone byte-wrapping pager. Exercises the API and is what the PTY test drives. Real clients (e.g. mat) supply a width-aware `render_line`. |
| `tests/pty_test.c` | drives `paige-demo` inside a real pty via `forkpty`, feeds keystrokes, asserts on the rendered screen. |

Key model: `view` tracks the top of the screen as a (logical line, visual
segment) pair, because one logical line may wrap to several visual rows. All
movement (`move_down`/`move_up`/`goto_line`/`G`) operates in that space and asks
the render callback for segment counts on demand.

## 3. Current state — what works

- Navigation: `j`/`k`/`↑`/`↓`, space/`f`/`b` (page), `d`/`u` (half-page),
  `g`/`G` (top/bottom), `q`/`Ctrl-C` (quit).
- **Live incremental goto-line**: type digits to jump as you type; a pause longer
  than the entry timeout (default ~600ms, see §6) commits the running number and
  starts a fresh one, so `1`<pause>`6` lands on line 6 while `16` lands on
  sixteen. Status line shows the pending number during entry.
- `quit_if_one_screen`: print plainly and return when the content fits.
- SIGWINCH resize, reverse-video status line with title + current line / `(END)`.
- Tested via the pty harness on Linux, macOS, FreeBSD (CI). ASan/UBSan clean.

## 4. Build / test

```sh
make            # builds ./paige-demo + build/libpaige.a
make test       # runs tests/run.sh (the pty integration test)
make asan       # ASan/UBSan build of the demo + test
```

CI (`.github/workflows/ci.yml`): lint (clang-format **19.1.7**, LLVM base, see
`.clang-format`), build matrix `{ubuntu,macos}×{gcc,clang}` with
`-Wall -Wextra -Wshadow -Wconversion -Werror`, a FreeBSD VM job, and sanitizers.
`all-jobs` is the aggregate gate. Match clang-format 19.1.7 exactly — minor
version skew reflows differently.

## 5. Roadmap — what's left (prioritized)

### P1 — the features that make it a *real* pager
- **Search** (`/`, `?`, `n`, `N`). The single biggest gap. Design sketch: add an
  optional host hook to fetch raw line text (or reuse `render_line` and strip
  ANSI), scan from the current position, move the view to the hit, and highlight
  matches in `draw()`. Needs: incremental input (reuse the `paige_term_key_timed`
  / line-entry pattern from digit-goto), wrap-around, "pattern not found" status,
  case-smart matching. This is a Sprint-sized effort.
- **Horizontal scroll / chop mode** (`←`/`→`, and a no-wrap mode). Today a long
  line in a non-wrapping client is just truncated to the segment. mat already
  exposes `--chop-long-lines`; paige needs a horizontal offset in `view` and to
  pass it through the render path so chopped lines can be panned.

### P2 — niceties
- Goto-percent (`50%`), follow mode (`F`, tail -f style), marks (`m` / `'`),
  an `h` help overlay, mouse-wheel scroll, `:n`/`:p` for multiple documents.

### Perf — prove the thesis (not yet done)
- **No benchmark vs `less` exists yet.** A pager can't be hyperfine'd like cat,
  but the meaningful wins are measurable; see §7.

## 6. Timing & the digit-goto test (important gotcha)

The digit-goto entry timeout is configurable via `paige_opts.goto_pause_ms`
(`<=0` → default 600ms). `demo.c` reads it from the `PAIGE_GOTO_MS` environment
variable so the pty test can set a short value (150ms) and keep wide, robust
sleep margins.

Gotcha that bit us on fast machines (macOS arm64 CI): digit entry holds an open
*timed* read for `goto_pause_ms`. If a test writes the next keystroke before that
window closes, the keystroke is swallowed into the *previous* number (e.g.
`16` then a quick `1` becomes `161`). The test therefore (a) shortens the timeout
via `PAIGE_GOTO_MS`, (b) sleeps well past it (~400ms vs 150ms) between sub-tests,
and (c) drains the pty to a clean buffer before negative assertions. Keep this
discipline when adding goto/search tests: **always settle past the entry timeout
before the next interaction.**

Other test infra notes: the pty test self-times out via `alarm(15)` and force-
kills the child as a backstop so it can never hang CI. FreeBSD needs
`<sys/ioctl.h>`/`<termios.h>` included **before** `<libutil.h>` (for
`struct winsize`).

## 7. Performance benchmarking plan (TODO: `bench/pager.sh`)

paige's design bet is lazy mmap + render-on-demand. The honest, measurable
comparisons against `less` (and `ov`/`moar`):

1. **Time-to-first-screen** on a multi-GB file — paige should be ~instant where
   `less` indexes/reads first. Drive via the pty harness (or `script`/`expect`):
   launch, wait for the first frame, record elapsed.
2. **Jump-to-end latency** — `G` (paige) vs `less +G` on a huge file.
3. **Resident memory** — `/usr/bin/time -v` peak RSS while paging a huge file
   (mmap + lazy index vs buffering).
4. **Throughput microbench** — a host that renders N screens then quits, timed,
   to isolate engine cost from terminal I/O.

Never benchmark on `/dev/zero` (tmpfs zero-pages flatter any reader); use random
data. This mirrors mat's benchmarking discipline.

## 8. Reference implementations to study

Not yet cloned. Recommended to clone into `.docs/refs/` (gitignored) when working
on search / horizontal-scroll / perf:

| ref | why | clone |
|---|---|---|
| **less** | the C canonical; study its search, jump table, and line-position model | `git clone https://github.com/gwsw/less .docs/refs/less` |
| **ov** | Go, modern, feature-rich lazy loading + search UX | `git clone https://github.com/noborus/ov .docs/refs/ov` |
| **moar** | Go, clean modern take on paging + search | `git clone https://github.com/walles/moar .docs/refs/moar` |
| **pspg** | table/column pager — ideas for structured content | `git clone https://github.com/okbob/pspg .docs/refs/pspg` |

After cloning, write a short audit here (`.docs/audits/`) of what to borrow
(less's search state machine and goto-mark; ov/moar's incremental search UX) and
what to skip, the way mat seeded itself from cat/bat.

## 9. Conventions

- C11 + POSIX. Portable across Linux, macOS, FreeBSD; build under GNU make and
  bmake. Guard platform headers (`pty.h` Linux / `util.h` macOS / `libutil.h`
  BSD).
- Warnings are errors in CI (`-Wconversion` included — mind implicit narrowing,
  especially `ssize_t`/`size_t`/`int`).
- clang-format (LLVM base, `IndentWidth: 4`, `ColumnLimit: 80`); avoid multi-line
  ternaries and trailing comments that push past 80 — they reflow unpredictably.
- Keep the public header (`include/paige.h`) tiny and stable; new behavior is
  opt-in through `paige_opts`, defaulting to current behavior on a zeroed struct
  (hosts brace-init `paige_opts`).
- Commits: terse, imperative, no co-author/generated trailers.
