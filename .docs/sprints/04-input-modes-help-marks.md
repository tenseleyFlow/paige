# Sprint 4: Input Modes, Help, Marks

## Goal

Add the interaction polish expected from a finished pager without changing
paige's role as an engine.

## Enumerated Targets

1. Introduce explicit modes if Sprint 2 did not already do so.
   Viewing, search entry, percent-goto entry, mark entry, and help should be
   represented directly instead of inferred from scattered flags.

2. Add a small reusable input buffer.
   Search and percent-goto need editing. Keep it byte-oriented unless the project
   commits to full UTF-8 editing.

3. Implement goto-percent.
   `50%` should move near the middle of known content. For unknown total line
   counts, decide whether to scan lazily to EOF, reject with status, or require an
   optional host line-count hook.

4. Implement marks.
   Support `m<char>` to set a mark and `'<char>` to jump. Store enough state to
   restore logical line, visual segment, horizontal offset, and possibly document
   identity if multidoc lands later.

5. Add special marks.
   Consider `''` for previous position, `'^` for top, `'$` for bottom, and `'.`
   for current position, following less where useful.

6. Implement `h` help.
   Treat help as normal paged content with saved/restored view state. Keep the
   help text short and generated from the actual key bindings where practical.

7. Add transient status messages.
   Unknown commands, missing marks, unavailable search, wrap notices, and mode
   toggles should produce concise messages without corrupting the main view.

8. Consider mouse-wheel scroll.
   Add only if terminal decoding can stay small and tests remain reliable. Wheel
   events should map to the same movement functions as keyboard scroll.

9. Add tests.
   Cover percent goto, marks, previous-position mark, help enter/exit, status
   messages, and any mouse support if included.

10. Add friendly discoverability.
    Consider a concise command hint in the status line, mode-specific help text,
    and an optional `?` help search if it does not conflict with backward search.

11. Add jump history as a user feature and a performance tool.
    Large jumps, search jumps, percent jumps, and marks should leave a return path
    that helps users explore without getting lost.

## Pitfalls To Avoid

1. Do not let digit-goto and percent-goto fight over the same pending number
   state.

2. Do not make `h` unavailable because horizontal scroll wants vim-style `h`.
   Pick one binding intentionally.

3. Do not store marks as only logical line numbers if wrapped segment or
   horizontal offset matters.

4. Do not scan to EOF for percent goto on a huge unknown stream without clear
   status and a way out.

5. Do not build a large command language. This sprint is for pager basics, not a
   full less clone.

6. Do not add status/help redraws that repaint the whole file body when only the
   footer changed.

## Definition Of Done

1. `h` opens help and `q` or `Esc` returns to the saved view.

2. Percent goto works or reports a clear unsupported status for unknown length
   documents.

3. Marks can be set and revisited after scrolling, paging, search, and resize.

4. Previous-position behavior works for large jumps and search jumps.

5. Transient statuses are tested in the PTY harness.

6. Existing navigation, search, and chop tests still pass.

7. `make test` and sanitizer test pass.

8. Status-only changes can be rendered or measured without full body redraw where
   the current renderer supports it.

9. Help text calls out paige-specific features, not just less-compatible keys.

## General Guidelines

1. Keep help content inside the repo and versioned with key behavior.

2. Let modes own key interpretation, but reuse movement and draw functions.

3. Borrow less's mark semantics selectively; do not copy its global mark model.

4. Prefer discoverability in the status line over extra commands.

5. Use friendliness to reduce cognitive load: show what paige is doing and how to
   get back.
