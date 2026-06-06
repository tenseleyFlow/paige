# Benchmark Baseline

Captured 2026-06-05 on a 64 MiB real-source corpus (this repo's own `.c/.h` +
README + a test, tiled — realistic line lengths and structure, never random or
sparse, never `/dev/zero`):

```sh
sh bench/pager.sh --size-mb 64 --runs 2
```

System context:

```text
Linux dorado 5.15.0 (FreeBSD 15.0 host, Linux compat) x86_64
```

## How to read this

- `first_ms` is **end-to-end** time-to-first-byte: it includes the *demo host's*
  eager file slurp + line index (O(file size)), which is the demo's design, not
  the engine's. It is **not** isolated engine first-paint.
- `render=`, `segments=`, `search_lines=` are paige's own stat counters and
  **do** isolate engine work (they are `-1` for `less`/`ov`/`moar`, which don't
  emit them). These are the honest laziness signal.
- `rss_max` is `getrusage(2)` `ru_maxrss`; units vary by OS.

## Results (run=1 of each)

```text
bench mode=first  name=paige            first_ms=100.838 rss_max=198704 render=48  search_lines=0       segments=23  status=ok
bench mode=jump   name=paige            first_ms=102.011 render=94  search_lines=0       segments=46  status=ok
bench mode=search name=paige-late       elapsed_ms=25.342 render=464 search_lines=2225574 segments=438 status=ok
bench mode=search name=paige-nohit      elapsed_ms=50.496 render=554 search_lines=4379953 segments=529 status=ok
bench mode=first  name=paige-huge-wrap  first_ms=81.402  render=2   search_lines=0       segments=23  status=ok
bench mode=first  name=paige-huge-chop  first_ms=79.277  render=3   search_lines=0       segments=1   status=ok
bench mode=first  name=less             first_ms=1.646   render=-1  search_lines=-1      segments=-1  status=ok
bench mode=jump   name=less             first_ms=2.618   render=-1  search_lines=-1      segments=-1  status=ok
bench mode=search name=less-late        elapsed_ms=0.183 render=-1  search_lines=-1      segments=-1  status=ok
bench mode=first  name=less-huge        first_ms=1.638   render=-1  search_lines=-1      segments=-1  status=ok
```

`ov` and `moar` were not installed on this host (`status=missing`); the script
benchmarks them when present.

## What it shows (and doesn't)

- **The engine is lazy.** `render=48`/`segments=23` on first paint are identical
  to the 1 MiB run — engine work is O(screen), independent of file size. On the
  64 MiB single-line wrap case it is `render=2`/`segments=23` (O(visible), not
  O(line) — the windowed-render fix).
- **`less` beats the demo on first paint** (1.6 ms vs ~100 ms), and that is
  reported honestly: the demo *host* slurps and indexes the whole file up front,
  while `less` mmaps and indexes lazily. The gap is the demo's host policy, not
  the paige engine — the counters make this visible. A host like `mat` that
  indexes lazily would not pay it.
- **Search counters are exposed**, not hidden: a committed full-document search
  on 64 MiB scans millions of lines (`search_lines`), bounded per keystroke
  while typing (see the test suite). Wall-clock alone would have hidden this.

This is a script-health + engine-laziness baseline, not a marketing comparison.

## Enforcing gate

The numbers above are the human-readable record. The machine-checked regression
gate lives in `bench/baseline.json` and runs as `make bench-check` (CI job
`perf-gate`). It asserts the **deterministic** counters — `render`/`segments` on
first paint, jump-to-bottom, and the huge-line wrap/chop cases — as upper bounds.
These are exact at a fixed 24×80 tty and constant across file size, so the gate is
noise-free (unlike wall-clock) and a breach means an algorithmic regression: the
huge-wrap bound (`render ≤ 8`) is what would have caught the O(line) wrap bug.
`first_ms`, `rss_max`, and `search_lines` are **not** gated — they vary with host
indexing policy, OS, and corpus size. Re-record with `sh bench/check.sh --show`.

## Parity finding: mat-as-host vs less (the engine is already at parity)

The `first_ms` gap above (paige-demo ~130–180 ms vs less ~5 ms on 64 MiB) is the
**demo host's** eager `slurp()` + full `index_lines()` prologue, not the engine.
To prove that, the bench now drives **mat** — the production host, which indexes
lazily (mmap + on-demand newline index) — as a subject (`mat --paging=always
--decorations=never --color=never`, full runs only; excluded from `--smoke`).

Measured on the 64 MiB real-source corpus (this FreeBSD box):

| subject | first paint | jump-to-bottom (`G`) | why |
|---|---|---|---|
| paige-demo | ~130–180 ms | ~0.06 ms | eager slurp+index up front; has `line_count` so `G` is O(screen) |
| **mat** (lazy host) | **~21–37 ms** | **~1190 ms** | O(screen) first paint; **no `line_count` → `G` forward-scans to EOF, O(file)** |
| less | ~5–52 ms | ~43 ms | mmap + lazy line-position cache; seeks to EOF for `G` |

Two conclusions:

1. **First paint is already at parity.** mat (~21–37 ms) sits with less (~5–52 ms);
   the demo's ~130–180 ms is purely its eager prologue. The engine renders a
   screenful regardless of file size — confirmed by the gated counters above.
2. **One real engine gap remains, and it is navigation, not first paint.** A host
   with no `line_count` (mat) takes the "unknown length" branch of `goto_bottom()`
   and forward-scans to EOF on `G` — ~1.19 s on 64 MiB vs less's ~43 ms. The demo
   avoids it only because it supplies `line_count`. This motivates the optional
   `seek_end` hook so a seekable host can reach the bottom in O(screen).

Note: the pty bench driver's jump/search timing was fixed alongside this — it used
to send the keypress on the first output byte, mistaking the initial frame's tail
for the response and reporting a slow `G` as ~0 ms (mat read "0.04 ms" before the
fix). It now waits for the initial frame to settle, then times the response, so a
stalled redraw is measured honestly. Driving mat through the minimal bench pty can
stall briefly on mat's startup terminal probe; the driver's 15 s timeout caps it,
which is why mat is a full-run-only subject and never part of the gate.

### Campaign result (both gaps closed)

Acting on the two conclusions:

- **The demo is now lazy** (mmap + on-demand `memchr` index). Its first paint on
  64 MiB dropped from ~130–180 ms to **~1.5 ms** — at/below `less` (~3.5 ms) — and
  `host_indexed` stays a screenful (26) regardless of file size, gated on the
  `first.*` cases so a regression to eager indexing trips CI.
- **The `seek_end` hook closed the `G` gap** for hosts without `line_count`: it
  takes them from forward-scan-render (render=100095 / ~1.19 s on 64 MiB) to a
  jump-and-fill of **render=94 / ~37 ms** — the EOF-find cost only, on par with
  `less` (~43 ms). The demo keeps a (now-lazy) `line_count` so `%` still works and
  its `G` rides that path; `seek_end` is exercised via `PAIGE_NO_COUNT`.

Net: paige is at first-paint parity with `less` and within noise on jump-to-bottom,
with every deterministic signal gated in CI.
