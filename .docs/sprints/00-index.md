# Sprint Index

This sprint set takes paige from the current compact pager engine to a polished
standalone product. Each sprint is scoped so it can land independently while
preserving paige's core contract: the host owns content, paige owns terminal
interaction, and rendering stays lazy.

## Current State

1. `include/paige.h` exposes a small C API: `paige_doc`, `paige_opts`,
   `paige_emit`, and `paige_run`.

2. `src/pager.c` tracks viewport position as `(logical line L, visual segment S)`
   and renders only the visible screen through the host callback.

3. `src/term.c` owns `/dev/tty`, raw mode, alternate screen, resize handling, and
   basic key decoding.

4. `src/demo.c` is a byte-wrapping reference client.

5. `tests/pty_test.c` verifies interactive navigation and digit-goto through a
   real pty.

## Product Finish Line

1. Competitive speed: first screen before full-file work, no avoidable full-line
   copies, allocation-conscious search, and measured terminal output.

2. Search: `/`, `?`, `n`, `N`, incremental entry, wrap-around, smart-case,
   visible match highlighting, not-found status, and PTY coverage.

3. Horizontal scroll and chop mode: no-wrap mode, left/right movement, search-hit
   reveal, status indication, resize behavior, and long-line tests.

4. Pager niceties: percent goto, marks, help overlay, transient messages, optional
   mouse wheel, and clearer status text.

5. Live content: follow mode with host-owned refresh semantics, and optional
   multiple-document navigation if it fits the engine boundary.

6. Performance proof: benchmarks for first screen, jump to end, RSS, huge lines,
   search, and engine micro-cost against `less`, `ov`, and `moar` where possible.

7. Product differentiation: progressive status, long-line tools, semantic jumps,
   reversible filters, and optional performance introspection.

8. Release polish: documented API, examples, stable build/test workflow,
   portability checks, sanitizer cleanliness, and a release checklist.

## Performance North Stars

1. Time to first screen is the headline metric.

2. Long-line operations should scale with visible width where possible, not total
   line length or horizontal offset.

3. Search should find the first visible or nearby hit before any full-document
   scan is required.

4. Terminal output should be tracked by rows damaged, bytes emitted, and writes
   per interaction.

5. Background indexing, counting, highlighting, and searching must never block the
   first frame.

## Sprint Files

| sprint | file | outcome |
|---|---|---|
| 0 | `00-kickoff-baseline.md` | Capture the verified starting point and Sprint 1 backlog. |
| 1 | `01-api-and-test-foundations.md` | Make room for search/chop without bloating the API. |
| 2 | `02-search.md` | Implement real pager search. |
| 3 | `03-horizontal-scroll-and-chop.md` | Implement no-wrap mode and horizontal movement. |
| 4 | `04-input-modes-help-marks.md` | Add interaction polish: modes, help, marks, percent goto. |
| 5 | `05-follow-streaming-multidoc.md` | Add follow/live-content behavior and decide multidoc scope. |
| 6 | `06-performance-benchmarks.md` | Prove the lazy-rendering performance thesis. |
| 7 | `07-innovation-differentiators.md` | Add distinctive UX and friendliness features. |
| 8 | `08-hardening-release-polish.md` | Finish docs, portability, packaging, and release readiness. |

## Cross-Sprint Rules

1. Keep the public API small and append-only unless the repo explicitly resets API
   stability before a release.

2. Preserve zero-initialized `paige_opts` behavior.

3. Do not add runtime dependencies.

4. Avoid full-document rendering or indexing inside the engine.

5. Put new pure logic in testable units when practical; keep PTY tests for real
   terminal behavior.

6. Keep Linux, macOS, and FreeBSD portability in every sprint.

7. Treat `-Wall -Wextra -Wshadow -Wconversion -Werror` as the normal build target.

8. Update `README.md` or `.docs/HANDOFF.md` whenever a sprint changes public
   behavior or future work assumptions.

9. Add performance notes or counters when introducing work that could affect first
   paint, search, long-line rendering, or terminal output volume.
