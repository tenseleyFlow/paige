# Sprint 3: Horizontal Scroll And Chop

## Goal

Add no-wrap/chop mode and horizontal movement while preserving the current wrapped
viewport model.

## Enumerated Targets

1. Add horizontal state to the view.
   Extend the internal view from `(L, S)` to include a horizontal cell offset.
   The offset should be meaningful only in chop mode.

2. Add a public option for wrap behavior.
   Preserve current wrapping as the zero-value default. Add an opt-in chop mode
   through `paige_opts` or the extended render request chosen in Sprint 1.

3. Pass horizontal context through rendering.
   The host must know whether paige wants wrapped segments or a chopped slice.
   Avoid making the host render entire huge lines just so paige can discard the
   left side.

4. Implement horizontal commands.
   Support left and right arrows. Consider `h` and `l` only if they do not
   conflict with help. Support larger jumps with shifted arrows or a documented
   repeated movement width.

5. Clamp and reset intentionally.
   Clamp negative offsets to zero. Decide whether `g`, `G`, page movement, search,
   and resize preserve horizontal offset. Document the decision and test it.

6. Add visible continuation markers.
   A simple left/right marker is enough. It should not corrupt host-rendered ANSI
   state or consume confusing content columns.

7. Make search reveal horizontal hits.
   In chop mode, a search hit outside the visible slice should adjust horizontal
   offset so the active match is visible.

8. Update `paige-demo`.
   Add a demo flag or environment-controlled mode for chop behavior so PTY tests
   can exercise it.

9. Add tests for long lines.
   Cover initial chop, right scroll, left scroll, clamp at zero, resize, search
   reveal, page movement, and a very long single line.

10. Plan for display-column checkpoints.
    A simple first version may use byte slices for the demo, but the engine design
    should be able to seek from display column to byte offset without scanning from
    the start of a huge line on every redraw.

11. Track horizontal-render cost.
    Add counters or tests that make it visible when horizontal scrolling scales
    with total line length instead of visible width.

## Pitfalls To Avoid

1. Do not mix visual segment `S` semantics with horizontal offset. Wrapped mode
   and chopped mode need clear rules.

2. Do not use byte offsets as terminal cell offsets without documenting the
   limitation. If full Unicode width support is not in this sprint, keep the API
   ready for cell-based offsets.

3. Do not require clients to allocate an unbounded rendered line for chop mode.

4. Do not leave ANSI styles open when clipping colored output.

5. Do not let horizontal scroll break `quit_if_one_screen`; that mode should
   still print plainly according to current behavior unless explicitly changed.

6. Do not let search highlighting and horizontal slicing apply in the wrong order.

7. Do not copy or tokenize an entire pathological line just to show one viewport
   slice.

8. Do not accept `O(horizontal_offset)` as the long-term horizontal scroll model.

## Definition Of Done

1. Current wrapped behavior is unchanged by default.

2. Chop mode displays one visual row per logical line.

3. Left/right movement works in a real pty.

4. Horizontal offset is visible in the status line or through clear scroll
   markers.

5. Search reveals off-screen matches in chop mode.

6. Very long lines remain responsive.

7. PTY tests cover chop and horizontal navigation.

8. `make test` and sanitizer test pass.

9. A huge-line test or benchmark demonstrates that chop mode remains responsive.

10. The implementation notes document whether the current line slicing is byte-,
    rune-, or cell-based.

## General Guidelines

1. Use `less` as the conceptual reference for `hshift`, but keep paige's state
   local and explicit.

2. Use `moar` as the performance reference for bounded huge-line rendering.

3. Treat table/frozen-column behavior from `pspg` as future inspiration, not part
   of this sprint.

4. Prefer simple, documented limitations over complex partial Unicode handling.

5. Make long lines a primary workload, not a bug farm after release.
