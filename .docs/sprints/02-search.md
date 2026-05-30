# Sprint 2: Search

## Goal

Implement the core feature that makes paige feel like a real pager: `/`, `?`,
`n`, and `N` with incremental input, smart-case matching, wrap-around, status,
and highlighting.

## Enumerated Targets

1. Add search state to the pager.
   Track the committed pattern, current entry buffer, direction, smart-case mode,
   active match line, active match byte range, not-found message, and the view
   snapshot used for `Esc` restore.

2. Implement raw-line scanning.
   Use the optional raw-line hook from Sprint 1. If the hook is absent, search
   should report that search is unavailable rather than guessing from rendered
   output.

3. Implement smart-case literal matching first.
   Case-insensitive search is used when the query has no uppercase ASCII. Any
   uppercase ASCII makes the query case-sensitive. Regex is out of scope unless a
   later API decision adds it explicitly.

4. Implement `/` and `?` input modes.
   `/` searches forward. `?` searches backward. Printable bytes edit the query,
   Backspace deletes, `Enter` commits, `Esc` restores the pre-search view, and
   `Ctrl-C` exits search or quits consistently.

5. Implement incremental movement.
   As the query changes, paige should move to the nearest match in the selected
   direction. Empty query restores the search origin while keeping the prompt.

6. Implement `n` and `N`.
   `n` repeats in the last committed direction. `N` repeats in the opposite
   direction. Repeating without a committed pattern should show a transient
   status message.

7. Implement wrap-around.
   A failed scan from the current point should wrap once and continue. Status
   should distinguish wrapped success from not found if the UI has room.

8. Implement visible highlighting.
   Highlight all visible matches for the current pattern when practical, and at
   minimum highlight the active match. The demo path must support highlighting in
   plain text.

9. Keep search bounded and interruptible.
   A long scan must not allocate proportional to the document. It should notice
   quit/resize where possible and leave the terminal in a valid state.

10. Keep the literal path allocation-conscious.
    The first search implementation can be simple, but it should not require a
    heap allocation per line for ASCII literal search.

11. Leave room for cancellable progressive search.
    If the first version is synchronous, isolate it so a later worker can search
    visible rows first, nearby chunks next, and the full document in the
    background.

12. Add search tests.
    Cover forward, backward, wrap, `n`, `N`, not found, smart-case, empty query,
    `Esc` restore, highlighted visible matches, and no raw-line hook behavior.

## Pitfalls To Avoid

1. Do not search visual segments; wrapping width should not change search hits.

2. Do not let incremental search consume the next command the way digit-goto can
   if tests fail to settle. Search tests need the same timeout discipline noted in
   `.docs/HANDOFF.md`.

3. Do not make `n` search from the same active match forever. Start after the
   current match for forward repeat and before it for backward repeat.

4. Do not highlight by injecting ANSI into already-styled host output unless the
   extended render contract makes that safe.

5. Do not scan past EOF forever when the host returns sparse or inconsistent raw
   line data.

6. Do not use locale-sensitive case folding in the first version. ASCII
   smart-case is predictable and portable.

7. Do not rescan rendered rows just to answer whether a match is visible. Track
   match locations separately from draw output.

8. Do not make every incremental keystroke restart an expensive full-document scan
   if a prefix or visible-result shortcut can answer quickly.

## Definition Of Done

1. `/`, `?`, `n`, and `N` work in `paige-demo`.

2. Search status is visible in the bottom line during entry and after failures.

3. Search wraps once and reports not found when no hit exists.

4. `Esc` from search restores the exact pre-search view.

5. Search does not break digit-goto behavior.

6. PTY tests cover the user-visible search flow.

7. Pure tests cover smart-case and matching edge cases.

8. `make test` and sanitizer test pass.

9. Search tests or counters show the literal fast path does not allocate per
   candidate line.

10. The search state can preserve match locations for repeat navigation without
    depending on the current rendered frame.

## General Guidelines

1. Model search after less conceptually: separate command-buffer state from
   committed search state.

2. Model UX after moar: incremental movement, clear footer, and abort-restore.

3. Keep the first matching engine boring. Literal substring search is enough.

4. Prefer explicit status messages over silent no-ops.

5. Optimize for first-hit latency before total-hit completeness.
