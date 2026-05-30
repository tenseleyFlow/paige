# Sprint 5: Follow, Streaming, Multidoc

## Goal

Support live content and optional multi-document navigation without making paige
own the host's storage model.

## Enumerated Targets

1. Define the engine boundary for follow mode.
   paige should not watch files itself unless the demo needs it. Prefer host hooks
   that let the engine ask for refreshed line availability and redraw when data
   changes.

2. Represent follow as a target, not a special scroll hack.
   Borrow moar's idea: following means the target is the current end. Scrolling up,
   searching, jumping to a mark, or entering help pauses follow.

3. Implement `F` follow mode.
   `F` enters follow. Any upward/manual navigation exits or pauses it. New content
   should keep the view pinned to the bottom when follow is active.

4. Handle partial final lines in the demo.
   Appending to a file that previously lacked a trailing newline should update the
   last logical line rather than creating corrupt display state.

5. Add a redraw wakeup strategy.
   The current key loop blocks in `paige_term_key`. Follow mode needs timed waits,
   host notifications, or a small poll loop that still handles resize and quit
   promptly.

6. Decide multi-document scope.
   If paige owns multiple docs, add a small optional `paige_run_many` wrapper and
   `:n`/`:p`. If the host owns doc switching, expose enough state or callbacks for
   the host to restart cleanly.

7. Preserve per-document state.
   Search pattern, marks, horizontal offset, and follow mode should either be
   document-local or explicitly global. Make the rule clear.

8. Add tests.
   Cover append while following, pause on scroll, resume with `F`, partial final
   line append, EOF shrink/reload behavior if supported, and multidoc switching if
   included.

9. Coalesce follow redraws.
   High append rates should not trigger one full terminal frame per appended line.
   Track update rate, dropped/coalesced frames, or redraw count in tests or debug
   counters.

10. Highlight newly appended content when cheap.
    This is a friendliness differentiator and makes follow mode easier to read.

## Pitfalls To Avoid

1. Do not block forever waiting for a key while follow mode expects redraws.

2. Do not put platform-specific file watching in the core engine unless there is
   a portable fallback.

3. Do not duplicate host storage inside paige just for follow mode.

4. Do not let follow mode consume CPU with a tight polling loop.

5. Do not make multi-document support force every single-document host through a
   larger API.

6. Do not leave marks pointing to the wrong document after switching.

7. Do not implement follow with a busy loop or fixed one-second latency if the
   host can provide a better wakeup.

## Definition Of Done

1. `F` works in `paige-demo` for a growing file or documented stream scenario.

2. Follow mode redraws without requiring a keypress.

3. Manual navigation pauses or exits follow predictably.

4. Partial final line append is tested.

5. Multidoc behavior is either implemented and tested or explicitly deferred with
   a documented host-side recommendation.

6. Existing search, chop, help, and mark behavior remains stable.

7. `make test` and sanitizer test pass.

8. Follow mode has a documented redraw cadence and does not repaint more often
   than necessary under bursty appends.

9. Appended-line highlighting is implemented or explicitly deferred.

## General Guidelines

1. Keep live-content behavior opt-in.

2. Favor host callbacks over engine-owned file watching.

3. Treat follow as navigation policy around the end of content, not as a separate
   renderer.

4. Make wakeups boring and portable before making them clever.

5. Prefer responsiveness plus coalescing over either latency spikes or terminal
   spam.
