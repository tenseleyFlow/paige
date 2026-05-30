# Sprint 6: Performance Benchmarks

## Goal

Prove paige's design thesis with repeatable benchmarks and guard against future
regressions.

## Enumerated Targets

1. Add `bench/pager.sh`.
   The script should create or accept large random-data fixtures, drive pagers in
   a pty, record timing, and print machine-readable results where practical.

2. Measure time to first screen.
   Launch paige, `less`, `ov`, and `moar` on the same large file and record time
   until the first rendered frame is visible.

3. Measure jump-to-end latency.
   Compare paige `G` against appropriate reference commands such as `less +G`.
   Record whether the benchmark includes initial indexing time.

4. Measure resident memory.
   Use platform-appropriate tools. Linux can use `/usr/bin/time -v`; macOS and
   FreeBSD need alternatives or documented omissions.

5. Add an engine microbench.
   Exercise render-on-demand for a fixed number of screens without terminal I/O so
   code changes can be compared locally.

6. Add huge-line benchmarks.
   Include one very long line and many moderately long lines. Measure wrapped
   draw, chop draw, search on cold data, and search on warm data.

7. Add search benchmarks.
   Benchmark smart-case literal matching separately from pager movement. Include
   no-hit, early-hit, late-hit, and repeated `n` cases.

8. Add benchmark documentation.
   Document fixture generation, expected noise, required tools, and how to avoid
   misleading results.

9. Add optional CI smoke coverage.
   Full benchmarks should not gate normal CI, but a tiny benchmark smoke test can
   verify the scripts do not rot.

10. Add terminal-output metrics.
    Capture writes per frame, bytes emitted, rows redrawn, and whether a scroll
    used full repaint or incremental terminal operations.

11. Add allocation metrics where practical.
    Search, draw, horizontal scroll, and first-screen paths should report or be
    measurable for allocation count and total allocated bytes.

12. Benchmark innovation workloads.
    Include structured logs, JSON lines, CSV/TSV-like wide data, ANSI-heavy logs,
    follow-mode append bursts, and pathological single-line files.

## Pitfalls To Avoid

1. Do not benchmark `/dev/zero` or sparse files. Use random data or realistic text.

2. Do not compare terminal I/O throughput when the goal is engine latency unless
   the benchmark says so.

3. Do not run full benchmarks in normal CI.

4. Do not hide cold-cache versus warm-cache differences.

5. Do not use reference pager options that change the user-visible behavior being
   compared unless the reason is documented.

6. Do not ignore huge single-line behavior; it is a common pager failure mode.

7. Do not rely only on wall-clock times. Capture enough counters to explain why a
   run improved or regressed.

8. Do not let benchmark fixtures require `.docs/refs/`; refs are local-only.

## Definition Of Done

1. `bench/pager.sh` exists and has usage text.

2. The benchmark suite can run paige and at least `less` when installed.

3. Results include first screen, jump to end, memory, huge line, and search data.

4. Benchmarks use non-sparse, non-zero fixtures.

5. `README.md` or `.docs/HANDOFF.md` points to the benchmark workflow.

6. A short baseline result is captured under `.docs/audits/` or another tracked
   docs file.

7. Normal `make test` remains fast.

8. A tracked baseline records first-screen, huge-line, search, and terminal-output
   numbers for the local implementation.

9. The benchmark docs state which comparisons target pager competitors and which
   target mat-style `cat`/`bat` throughput scenarios.

## General Guidelines

1. Keep benchmark scripts shell-first and dependency-light.

2. Prefer repeatable methodology over impressive numbers.

3. Separate terminal-driven benchmarks from pure engine benchmarks.

4. Record enough system context to make future comparisons honest.

5. Use benchmark failures to choose implementation work, not to produce marketing
   numbers prematurely.
