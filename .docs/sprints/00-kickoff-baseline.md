# Sprint 0: Kickoff Baseline

## Goal

Turn the sprint index into an executable starting point. This baseline records
what paige does today, what was verified before implementation starts, and what
Sprint 1 should tackle first.

Date: 2026-05-30

## Verified Baseline

1. `make CFLAGS="-std=c11 -O2 -Werror" && make test`
   Result: passed.

2. `make CFLAGS="-std=c11 -O2 -g -fsanitize=address,undefined" && make test`
   Result: passed.

3. PTY test coverage currently validates initial draw, `j`, `G`, `g`, live digit
   goto, paused digit-goto, quit, and quit-if-one-screen behavior through
   `paige-demo`.

## Current Implementation Shape

1. Public API is intentionally small.
   `include/paige.h` exposes `paige_doc`, `paige_opts`, `paige_emit`, and
   `paige_run`. The only content callback is `render_line(ctx, lineno, width,
   sink)`.

2. paige is host-driven and lazy.
   `src/pager.c` asks the host to render only lines needed for movement and draw.
   The viewport is `(logical line L, visual segment S)`.

3. Terminal ownership is isolated.
   `src/term.c` handles `/dev/tty`, raw mode, alternate screen, resize, timed key
   reads, and basic key decoding.

4. Rendering is simple and robust.
   `draw()` builds a whole-screen `outbuf` and writes one frame per key loop.

5. The demo is not performance-representative.
   `src/demo.c` slurps the full file into memory, indexes all line starts, and
   byte-wraps lines. It is a reference client and test target, not the desired
   engine storage model.

## Known Gaps Before Sprint 1

1. No raw-line access hook.
   Search cannot be implemented correctly without either a raw-line callback or a
   deliberately documented unavailable state.

2. No extended render request.
   Search highlighting, chop mode, horizontal offsets, and future semantic spans
   need context that the current `render_line` callback does not carry.

3. No explicit input modes.
   Digit-goto is implemented as a special loop in `paige_run`. Search, marks,
   help, and percent-goto will need cleaner mode handling.

4. No pure test target.
   All current behavior is validated through the PTY integration test. That is
   correct for terminal behavior but too coarse for search matching, clipping,
   input editing, and view math.

5. No performance counters.
   The engine does not currently expose or internally track render calls, rows
   drawn, bytes emitted, writes per frame, first-frame timing, or search scan work.

6. Full-frame redraw is the only rendering strategy.
   This is acceptable for the current engine, but it makes terminal-output volume
   a known performance target for later sprints.

7. Some navigation remains scan-heavy by design.
   `G` and overshot goto walk through logical lines via `segcount()`. This keeps
   the engine simple today but motivates progressive line-count/index support.

8. Allocation failures are mostly silent.
   `paige_emit()` and `outbuf` growth return early on failed `realloc()` without a
   surfaced engine error. Release hardening should revisit this.

## Performance Baseline Notes

1. First-screen engine work is conceptually lazy, but `quit_if_one_screen` may scan
   enough visual rows to prove whether content fits.

2. `paige-demo` is intentionally eager and should not be used to judge the final
   engine thesis against `cat`, `bat`, `less`, `ov`, or `moar`.

3. The engine currently writes one whole-frame buffer per draw. This gives a clean
   baseline for measuring future terminal damage tracking.

4. There is no benchmark harness yet. Sprint 6 owns the full benchmark suite, but
   Sprint 1 should add instrumentation seams so those measurements are possible.

## Sprint 1 Starting Backlog

1. Decide and document the minimal raw-line hook.

2. Decide and document an extended render request that can carry mode, width,
   horizontal offset, and match/highlight context while preserving the old callback
   fallback.

3. Add internal counters for render calls, emitted bytes, frame writes, and rows
   drawn.

4. Factor PTY test helpers before adding search and chop tests.

5. Add the first pure test target for small non-terminal logic.

6. Extend key decoding so printable input, `Enter`, `Esc`, Backspace, and editing
   keys can support future modes.

7. Update `paige-demo` only enough to exercise new API hooks in tests.

## Definition Of Done

1. Current build and tests pass under `-Werror` and sanitizer CFLAGS.

2. Current implementation state is documented.

3. Known Sprint 1 entry points are explicit.

4. Performance risks are named before implementation starts.
