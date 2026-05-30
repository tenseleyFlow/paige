# Reference Audit

This audit summarizes the local reference checkouts cloned under `.docs/refs/`.
The goal is not to copy their architecture, but to extract patterns that help
paige stay small, fast, dependency-free, and host-driven.

For the performance and product-differentiation pass, see
`.docs/audits/performance-innovation-audit.md`.

## Sources

1. `less`: `.docs/refs/less`
2. `ov`: `.docs/refs/ov`
3. `moar`: `.docs/refs/moar`
4. `pspg`: `.docs/refs/pspg`

## What To Borrow

1. Search should be its own state machine.
   `less` separates command input from committed search state with pattern and
   search flags. paige should do the same instead of bolting search directly into
   the main key switch.

2. Search should operate on raw logical lines.
   `less`, `ov`, `moar`, and `pspg` all search source text, then move the view to
   the hit. paige should add an optional raw-line hook instead of searching
   terminal-rendered segments.

3. Search UX should be incremental and reversible.
   `moar` restores the original scroll position on `Esc`; `ov` and `less` keep
   incremental state. paige should snapshot the view before `/` or `?`, update on
   edits, commit on `Enter`, and restore on `Esc`.

4. Smart-case literal search is the right default.
   `ov`, `moar`, and `pspg` all support variants of smart-case. paige should use
   case-insensitive search unless the query contains uppercase ASCII. Regex can be
   a later opt-in feature.

5. Horizontal scroll must be part of the view model.
   `less` uses `hshift`; `ov` tracks `scrollX`; `moar` uses `leftColumnZeroBased`;
   `pspg` parameterizes render panes by source offsets. paige should add a
   horizontal offset next to `(L, S)`, not hide it in the demo client.

6. Search should reveal off-screen horizontal matches.
   `less`, `ov`, and `moar` shift horizontally to make a found match visible.
   paige should do this in chop mode.

7. Huge lines need bounded rendering.
   `moar` avoids tokenizing/rendering entire huge lines when only a slice is
   visible. paige should ensure chop mode never forces the host or engine to
   format unbounded bytes for one screen row.

8. Tests should combine PTY goldens with pure unit tests.
   `less` has replayed terminal tests; `ov` and `moar` have focused unit and
   benchmark coverage. paige should keep the PTY harness, but add pure tests for
   search, matching, clipping, and command decoding.

9. Follow mode should be target-driven.
   `moar` represents follow as a target at the maximum line and clears it when
   the user scrolls up. paige can borrow that without owning file watching.

10. Help can be normal paged content.
    `moar` treats help text as a reader with saved/restored view state. paige can
    implement `h` with a synthetic doc instead of a separate renderer.

## What To Avoid

1. Do not import global-state-heavy designs.
   `less` and `pspg` are mature but have extensive globals. paige should keep a
   single pager state object and pass it explicitly.

2. Do not add curses or runtime dependencies.
   `pspg` depends on curses for its UI. paige should keep POSIX termios plus ANSI.

3. Do not make regex the first search milestone.
   Literal smart-case search is enough for the first polished version and avoids
   dependency, portability, and UX ambiguity.

4. Do not search ANSI-rendered output as the primary path.
   It is width-dependent, styling-dependent, and hard to map back to host data.

5. Do not let background or long searches block input forever.
   Large scans need visible status and an interrupt path. A synchronous first
   version is acceptable only if it can stop on quit/resize and never allocates
   proportional to full document size.

6. Do not make multi-document support central to the engine too early.
   paige is an engine. If `:n` and `:p` are added, keep them either host-driven or
   behind a small optional wrapper.

## Key Reference Files

1. `less`: `search.c`, `pattern.c`, `command.c`, `position.c`, `jump.c`, `mark.c`,
   `input.c`, `line.c`, `decode.c`, `cmdbuf.c`, `lesstest/`.

2. `ov`: `oviewer/search.go`, `input_search.go`, `search_move.go`, `store.go`,
   `document.go`, `reader.go`, `draw.go`, `move_leftright.go`, `mark.go`,
   `doclist.go`, `benchmark_test.go`.

3. `moar`: `internal/pagermode-search.go`, `internal/search/search.go`,
   `internal/search-linescanner.go`, `internal/screenLines.go`,
   `internal/linewrapper.go`, `internal/inputbox.go`, `internal/pager.go`,
   `twin/keys.go`, `internal/*_test.go`.

4. `pspg`: `src/pspg.h`, `src/pspg.c`, `src/table.c`, `src/linebuffer.c`,
   `src/print.c`, `src/commands.c`, `src/inputs.c`, `tests/pg_proc.txt`,
   `tests/*.csv`.
