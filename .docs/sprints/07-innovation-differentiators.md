# Sprint 7: Innovation Differentiators

## Goal

Turn paige from a capable pager engine into a noticeably friendlier and more
modern product surface. This sprint should only build on measured core behavior;
it must not compromise first-screen latency or the host-owned content contract.

## Enumerated Targets

1. Add progressive status language.
   Show what paige is doing when work is incomplete: indexing, searching,
   following, estimating line count, rendering a long line, or waiting for host
   data.

2. Add a search result overview.
   Start small: show current hit and known hit count. If the search/index model
   supports it, add compact match-distribution markers in the status line or a
   sidebar.

3. Add semantic jump hooks.
   Provide optional host callbacks or command plumbing for jumps such as next
   error, next warning, next timestamp, next JSON key, next diff hunk, or next
   section. paige should define the interaction, not own every parser.

4. Add long-line focus tools.
   Consider a column ruler, jump to next horizontal segment, collapse/expand long
   lines, and a focused view for the current logical line.

5. Add reversible filters.
   A filter should be a view over original line identities when host support
   exists. Users should be able to return to the original position without losing
   search state or marks.

6. Add a performance/debug panel.
   A hidden `:perf`-style view or debug status should show first-frame time,
   render time, terminal bytes, rows damaged, search throughput, and cache/index
   progress.

7. Add wide-data friendliness.
   For hosts that expose structured columns, consider fuzzy column switching,
   pinned columns, full-cell peek, and auto-fit selected column behavior.

8. Add better follow readability.
   Highlight appended lines, show update rate, and make pause/resume state obvious.

9. Add tests for differentiators.
   Cover progressive statuses, semantic-jump fallback, long-line tools, filter
   restore, perf panel rendering, and any structured-data affordances that land.

## Pitfalls To Avoid

1. Do not turn paige into a parser framework. Structured features must be optional
   and host-extensible.

2. Do not block first paint on semantic indexes, search counts, minimaps, filters,
   or syntax highlighting.

3. Do not introduce background work that cannot be cancelled or deprioritized.

4. Do not make the status line noisy. Progressive status should explain useful
   uncertainty, not create constant churn.

5. Do not add a feature that only works for mat unless the generic hook is clear.

6. Do not let debug/perf UI affect normal-frame performance when disabled.

## Definition Of Done

1. At least two product differentiators are implemented and documented, or the
   sprint records why they were deferred.

2. Progressive status exists for at least one long-running or incomplete operation.

3. Optional host-backed features have clear unavailable-state messages.

4. Any background innovation work is cancellable, coalesced, or bounded.

5. PTY or pure tests cover the features that land.

6. Benchmarks show no regression to first-screen latency or basic navigation.

7. `make test` and sanitizer test pass.

## General Guidelines

1. Prefer small, memorable features over a long list of half-finished commands.

2. Make paige explain itself when it is estimating, indexing, or intentionally
   doing less work for speed.

3. Keep innovation host-driven where data semantics are involved.

4. Use performance counters to keep friendly features honest.
