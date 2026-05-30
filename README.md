# paige

A small, fast, dependency-free terminal **pager engine** in C — the bespoke
pager behind [`mat`](https://github.com/tenseleyFlow/mat), built so it can be
reused on its own.

paige owns the terminal (raw mode, alternate screen, scrolling, keys, resize,
status line). It does **not** own the content: the client supplies a callback
that renders one logical line into visual segments for a given width. paige only
ever calls it for the lines it needs to show, so paging a multi-gigabyte file
never renders more than a screenful — it's lazy by construction.

Keyboard input is read from `/dev/tty`, so the content can be piped in.

## API

```c
#include <paige.h>

int my_render(void *ctx, size_t lineno, int width, paige_sink *sink) {
    // emit the visual segments of logical line `lineno` (wrapped to `width`)
    // via paige_emit(); return the segment count, or 0 at end-of-document.
}

paige_doc doc  = { .ctx = ..., .render_line = my_render, .title = "file" };
paige_opts opts = { .quit_if_one_screen = 1 };
paige_run(&doc, &opts);   // 0 on quit, -1 if there's no terminal
```

Optional hooks keep the basic API small while giving richer clients precise
control over search, layout, live content, and performance counters:

- `raw_line(ctx, lineno, out)` exposes source bytes for search and semantic
  features without inspecting ANSI-rendered output.
- `line_count(ctx, out)` exposes a known logical line count for percentage jumps;
  if unset, `%` reports that percent jumps are unavailable.
- `refresh(ctx)` lets a host report live-content changes while follow mode is
  active; paige redraws only when it returns nonzero.
- `render_line_ex(ctx, req, sink)` receives a `paige_render_req` with draw context
  such as wrap mode, horizontal offset, and match spans. If unset, paige falls
  back to `render_line`.
- `paige_opts.stats` can point at a `paige_stats` struct; paige zeroes and fills
  it with basic render/write/search counters during `paige_run`.
- `paige_opts.chop_long_lines` requests no-wrap display through
  `render_line_ex`; zero keeps the default wrapping behavior.
- `paige_opts.follow_poll_ms` controls follow-mode refresh cadence; `<=0` uses
  the default ~250ms poll. Idle follow polls do not repaint unless `refresh`
  reports a change. Multiple-document switching and appended-line highlighting
  remain host-owned: run paige separately per document or render new content
  distinctly in the client if desired.

## Build

```sh
make            # builds build/libpaige.a and the paige-demo binary
make examples   # builds examples/memory
make test       # PTY-driven interactive tests
make bench-smoke # tiny benchmark script health check
./paige-demo FILE   # a minimal standalone pager
./examples/memory   # embedded static document with raw search + chop mode
```

No external dependencies. C11 + POSIX (termios), portable across Linux, macOS,
and the BSDs.

For local performance work, run `sh bench/pager.sh --help`. Full benchmarks are
manual and fixture-backed; normal CI/test runs stay fast.

If `/dev/tty` or terminal output is unavailable, `paige_run` returns `-1`; hosts
should then print plainly or choose their own non-interactive fallback.

## Features

| feature | status | host requirement |
|---|---|---|
| Lazy render-on-demand | built in | `render_line` or `render_line_ex` |
| Smart-case literal search | built in | `raw_line` for searchable source bytes |
| Search highlighting | host-rendered | honor `paige_render_req.matches` |
| Chop / horizontal scroll | opt-in | `paige_opts.chop_long_lines` + `render_line_ex` |
| Percent goto | opt-in | `line_count` |
| Follow mode | opt-in | `refresh` |
| Marks, help, perf panel | built in | perf counters need `paige_opts.stats` |
| Benchmarks | manual | `bench/pager.sh` fixtures |

## Limits

- Layout is byte-oriented. paige does not claim Unicode display-width correctness.
- Search is literal smart-case substring search, not regex.
- ANSI styling is passed through host-rendered bytes; clipping styled output is a
  host concern unless the host implements `render_line_ex` carefully.
- Mouse support, multi-document switching, semantic jumps, filters, and appended
  line highlighting are intentionally host-owned or deferred.
- Follow mode polls through `refresh`; paige does not watch files itself.

## Keys
`q` quit · `j`/`k`/`↑`/`↓` line · space/`f`/`b` page · `d`/`u` half-page ·
`g`/`G` top/bottom · `/`/`?` search · `n`/`N` repeat search · `m<char>` set
mark · `'<char>` jump to mark · `''` previous position · `F` follow live content
until manual navigation · `P` performance panel · `h` help · `←`/`→`
horizontal scroll in chop mode ·
digits jump to a line as you type — the view follows each keystroke (`1`,`6` →
line 16), and a pause longer than ~600ms commits the number and starts a fresh
one (`1` … `6` → line 6). End a number with `%` to jump to that percentage when
the host provides `line_count`.

## License
MIT — see [LICENSE](LICENSE).
