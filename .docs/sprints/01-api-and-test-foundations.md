# Sprint 1: API And Test Foundations

## Goal

Prepare paige for search and horizontal scroll without losing the small engine
shape that already works.

## Enumerated Targets

1. Decide the minimal public API extension for raw source access.
   Search should scan logical source lines, not rendered ANSI segments. Add an
   optional hook at the end of `paige_doc`, such as a raw-line callback that
   returns bytes and length for a logical line.

2. Decide the minimal public API extension for extended rendering.
   Horizontal scroll and search highlighting need more context than
   `render_line(ctx, lineno, width, sink)`. Prefer one optional extended render
   hook over several feature-specific hooks. Keep the current callback as the
   fallback path.

3. Keep zero-init compatibility.
   A client that initializes `paige_doc` and `paige_opts` exactly as shown in the
   current README should still compile and behave the same unless the project
   deliberately declares a pre-1.0 API break.

4. Isolate feature-neutral pager state.
   Introduce a single internal state struct only if needed. The target is not a
   rewrite; it is to avoid stuffing search, chop, marks, and help globals into
   `pager.c`.

5. Normalize key handling before adding modes.
   Extend `term.c` so the pager can distinguish semantic commands from printable
   input bytes. Search and percent-goto need ordinary character entry, edit keys,
   `Enter`, and `Esc`.

6. Add pure test seams.
   New pure modules should be independently testable: search matching, smart-case,
   command/input editing, horizontal clipping, and view movement. Keep PTY tests
   for end-to-end terminal behavior.

7. Improve PTY test utilities before adding many cases.
   Factor repeated screen read, keystroke, settle, drain, and visible-line helpers
   so search and horizontal-scroll tests do not become fragile.

8. Extend the demo only as needed for test coverage.
   The demo should expose raw lines and the new render path because PTY tests run
   through `paige-demo`.

9. Add performance contracts to the API design.
   New hooks should let a capable host expose raw bytes, known line counts,
   approximate line counts, byte offsets, or line-length hints without requiring a
   basic host to implement them.

10. Add instrumentation seams.
    Keep counters internal at first: render calls, bytes emitted, rows drawn,
    search lines scanned, first-frame timing, and terminal writes. These counters
    will feed benchmarks and the later performance panel.

## Pitfalls To Avoid

1. Do not turn `include/paige.h` into a framework.

2. Do not search through `render_line` output as the main design.

3. Do not make the host allocate or copy every line for the engine.

4. Do not split files just to split files; extract only when the logic has a
   focused test or reuse point.

5. Do not assume `char` signedness or narrow `size_t` and `ssize_t` without an
   explicit bounds check.

6. Do not add Linux-only test behavior; FreeBSD and macOS remain first-class.

7. Do not add API hooks that imply paige owns the content storage model. Hosts
   should be able to stay callback-only.

8. Do not put exact line count or full EOF discovery on the startup path.

## Definition Of Done

1. The chosen API shape is documented in `include/paige.h` and `README.md`.

2. Existing callers can still use the old `render_line` path.

3. `paige-demo` exercises the new hooks.

4. PTY test helpers are ready for search and chop cases.

5. At least one pure test target exists for new non-terminal logic, or the sprint
   explicitly records why the next sprint will introduce it.

6. `make test` passes locally.

7. Sanitizer build remains clean.

8. The API plan identifies which operations are first-frame-safe and which may be
   progressive or unavailable.

9. Basic performance counters are available in tests or behind an internal debug
   path.

## Implementation Notes

Completed on 2026-05-30:

1. Added `paige_line`, `paige_match`, `paige_render_req`, and `paige_stats` to
   `include/paige.h`.

2. Appended optional `raw_line` and `render_line_ex` hooks to `paige_doc`.
   Existing `render_line` clients still work through the fallback path.

3. Appended optional `stats` output to `paige_opts`. paige zeroes it at
   `paige_run` start and fills render-call, frame, row, byte, and write counters.

4. Updated `paige-demo` to expose `raw_line`, exercise `render_line_ex`, and emit
   `paige_stats` when `PAIGE_STATS=1` is set.

5. Extended internal terminal decoding with text-entry support: printable bytes,
   `Enter`, `Esc`, Backspace, Delete, arrows, Home, and End.

6. Split PTY helpers into `tests/pty_helpers.h` and added `tests/unit_test.c` for
   pure key-decoder coverage.

7. Verification passed with `make CFLAGS="-std=c11 -O2 -Werror" && make test` and
   sanitizer CFLAGS.

Deferred or carried into later sprints:

1. `render_line_ex` carries chop, horizontal offset, and match context, but chop
   mode and highlighting remain Sprint 2 and Sprint 3 work.

2. Stats are coarse counters. Timing, row-damage, and terminal-diff metrics remain
   benchmark/performance-panel work.

3. The first pure test target covers key decoding only. Search matching, clipping,
   and view movement should get pure tests as those modules land.

4. Line-count, byte-offset, and line-length hint hooks were deliberately deferred
   until percent-goto, progressive indexing, or benchmark work proves the exact
   contract needed.

## General Guidelines

1. Prefer append-only structs and feature flags over incompatible redesigns.

2. Keep the current lazy invariant visible in reviews: one screen should not
   require rendering the full document.

3. Add comments only around new contracts or non-obvious terminal behavior.

4. When unsure between a perfect abstraction and a small concrete helper, choose
   the helper.

5. Treat first-screen latency as a contract, not a benchmark afterthought.
