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
