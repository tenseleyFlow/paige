# Performance And Innovation Audit

This audit is the second pass over `.docs/refs/`. The first audit focused on
feature architecture. This pass asks a sharper question: where can paige be
faster than existing pagers, and what product ideas can make it friendlier than a
traditional `less` clone?

## Competitive Thesis

paige should compete on latency first. A polished paige should feel instant on
large files because it renders the first viewport before doing any full-document
work. Expensive services should be progressive, cancellable, cached, and visible
to the user.

The core performance promise should be:

1. First paint reads only enough data for the viewport plus a small margin.
2. No full-file line count, syntax highlight, search index, or terminal probe is
   allowed on the first-paint path.
3. Regular files should use byte offsets and progressive newline indexes instead
   of per-line copies whenever the host can provide them.
4. Search should have an allocation-free literal fast path and a cancellable
   background path.
5. Long lines should be a first-class benchmark, not an edge case.
6. Terminal output should be measured by bytes written and rows damaged, not only
   CPU time.

## Reference Findings

### less

Good choices:

1. Lazy byte-position model in `ch.c`; opening a huge file does not require
   reading it.
2. First-screen work is close to `O(screen height)` unless options force more.
3. Search scans raw lines instead of rendered screen lines.
4. Terminal writes are buffered.

Performance left on the table:

1. `ch.c` uses fixed 8 KiB read buffers and no mmap or vectorized newline scan.
2. `linenum.c` uses a tiny reactive linked-list line-number cache, so deep
   jump-to-line remains scan-heavy.
3. `input.c` explicitly calls backward movement over long wrapped lines
   inefficient because it repeatedly walks forward from the raw-line start.
4. `search.c` allocates converted text and character-position buffers per
   searched line.
5. `position.c` shifts a screen-position array instead of using a ring buffer.
6. `output.c` and `screen.c` render through character-at-a-time attribute logic.

paige opportunities:

1. Progressive newline index built after first paint.
2. Long-line wrap checkpoints and column-to-byte seek.
3. Allocation-free literal search over chunks.
4. Retained terminal frame with row/span damage tracking.
5. Optional mmap/read-cache backend for regular files, if paige later owns any
   source implementation beyond the demo.

### ov

Good choices:

1. Chunked seekable-file architecture with on-demand chunk loading.
2. Parsed-line LRU cache.
3. Search can scan storage chunks without fully loading them.
4. Follow/watch modes are integrated into the document model.

Performance left on the table:

1. Startup reads a 10,000-line first chunk before first display.
2. Loaded lines are copied into separate byte slices.
3. Long-line rendering and word wrapping tend to parse whole lines.
4. Search lowercases or strips lines repeatedly and lacks a persistent result
   cache.
5. Incremental search can thrash goroutines on rapid input.
6. Draw often escalates to full synchronization and status changes can force
   immediate renders.

paige opportunities:

1. Viewport-first first chunk sized by terminal height, not a fixed 10,000 lines.
2. Byte-slab plus line-offset storage for demo/source helpers.
3. Cache raw, decoded, plain, lowercased, layout, and render layers separately.
4. Debounced cancellable incremental search with visible-first results.
5. Dirty flags and frame coalescing.

### moar

Good choices:

1. Async background reader.
2. Reader pause threshold limits eager memory/CPU use.
3. Long unwrapped lines have a render cap.
4. Search parallelizes large scans and has a substring fast path.
5. Terminal output is batched and has a small line-delta path.

Performance left on the table:

1. Seekable files may be counted before ingestion, duplicating I/O.
2. Syntax highlighting is whole-file and all-or-nothing above a size limit.
3. Wrapped huge lines are still fully processed.
4. Render path rebuilds many temporary structures every frame.
5. Search and filter repeatedly normalize strings.
6. Terminal diffing is coarse; most scrolls become full-frame writes.
7. Terminal background probing can add startup latency.
8. Tail mode polls on a one-second cadence.

paige opportunities:

1. No full pre-count before first paint.
2. Viewport-first syntax/ANSI/style processing where supported by the host.
3. Layout caches keyed by width, wrap mode, tab size, style generation, and search
   generation.
4. Search result metadata independent from rendered screen rows.
5. Scroll-aware terminal compositor.
6. Async terminal capability/theme probes after first paint.

### pspg

Good choices:

1. Column ranges make wide-table navigation practical.
2. Fixed rows and fixed columns are modeled as separate panes.
3. Progressive load exists for plain table input.
4. Search highlighting is lazy-cached per line.
5. Rendering only iterates visible rows.

Performance left on the table:

1. Horizontal scrolling scans from the start of each row to the horizontal offset.
2. Rendering is per-character with repeated width and style decisions.
3. CSV/TSV import is fully materialized and formatted before display.
4. Some structures have hard-coded 1024-column limits.
5. Search and sort often require complete load.
6. Memory stores full formatted rows and sometimes parsed copies at the same time.

paige opportunities:

1. Display-column checkpoints so horizontal scroll is proportional to visible
   width, not horizontal offset.
2. Structured table support as intervals/cells, not formatted strings.
3. Progressive CSV first paint with provisional widths.
4. Dynamic column vectors and binary-search column lookup.
5. Run-based rendering and damage tracking instead of curses-style window redraw.

## Performance North Stars

1. Time to first screen on a huge regular file.
   paige should beat `less`, `ov`, and `moar` by avoiding full-file work and fixed
   large first chunks.

2. Jump-to-line and jump-to-percent latency.
   paige should improve over time as the progressive newline index fills in.
   Unknown positions can be estimated immediately and refined.

3. Huge-line responsiveness.
   A 10 MB or 250 MB single line should not force a full-line copy, full-line
   tokenization, or `O(horizontal_offset)` scroll cost.

4. Search first-hit latency.
   Visible viewport first, nearby chunks next, full document in cancellable
   background workers.

5. Terminal bytes per interaction.
   Scrolling one row should not normally emit a full screen. Status-only updates
   should not repaint file rows.

6. Memory per GB of input.
   For seekable files and host support, keep metadata proportional to chunks and
   hot regions, not full rendered content.

## Product Innovation Gaps

1. Progressive status that explains work.
   Show messages like `first screen ready`, `indexing 18%`, `searching nearby`, or
   `exact line count pending` instead of freezing.

2. Search result minimap.
   A compact status/sidebar representation of match distribution would make huge
   files friendlier than classic `less` search.

3. Structured navigation.
   Add host-extensible jumps for timestamps, log levels, errors, JSON keys, CSV
   columns, diff hunks, stack frames, and code symbols.

4. Long-line focus tools.
   Provide a column ruler, jump to next overflow segment, collapse/expand long
   lines, and a focused full-line view.

5. Reversible filters.
   Filter should become a view over original line IDs with context, not a copied
   throwaway document.

6. Performance panel.
   A `:perf` or debug status overlay can show file backend, indexed percentage,
   cache use, render time, terminal bytes, and search throughput.

7. Friendly follow mode.
   Pause on manual scroll, resume at end, highlight appended lines, and show update
   rate or dropped/coalesced frames.

8. Progressive syntax or semantic highlighting.
   Highlight visible content first and avoid all-or-nothing size cutoffs.

9. Column-aware wide-data UX.
   Fuzzy column switcher, pin arbitrary columns, peek full cell, and auto-fit
   selected columns.

## Planning Implications

1. Performance cannot live only in the benchmark sprint. Every sprint should have
   a speed target and a regression guard.

2. API foundations need to leave room for source metadata, raw bytes, line counts,
   search hooks, and render requests without forcing every host to implement a
   file backend.

3. Search should start with a simple synchronous version only if the design leaves
   room for cancellable workers and cached result metadata.

4. Horizontal scroll should not land unless its design can become checkpointed and
   `O(visible width)` for long lines.

5. The product roadmap should include a dedicated innovation sprint before final
   release hardening.
