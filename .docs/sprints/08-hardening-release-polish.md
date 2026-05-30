# Sprint 8: Hardening, Release Polish

## Goal

Make paige ready to be used as an independent library, not just as a submodule
inside mat.

## Enumerated Targets

1. Freeze the public API for the first release.
   Review `include/paige.h` for naming, comments, zero-init behavior, ownership,
   lifetime rules, and error returns. Remove experimental fields or mark them
   clearly before release.

2. Strengthen terminal cleanup.
   Audit every exit path for raw-mode restoration, alternate-screen exit, cursor
   restore, and tty fd closure. Include signal-adjacent behavior where practical.

3. Harden allocation failures.
   `paige_emit`, output buffering, search buffers, mark storage, and test helpers
   should fail safely. Avoid silent partial state when an error can be surfaced.

4. Document portability limits.
   State exactly what is supported on Linux, macOS, and FreeBSD. Document any
   limitations around Unicode width, ANSI clipping, regex, mouse, or follow mode.

5. Expand examples.
   Keep `paige-demo`, but add at least one small example showing a host-owned
   document with raw-line search support and one example showing chop mode.

6. Improve README completeness.
   Include API overview, build/test commands, key bindings, feature matrix,
   fallback behavior when `/dev/tty` is absent, and benchmark workflow.

7. Add release artifacts where useful.
   Consider `make install`, `pkg-config` file generation, a version macro, and a
   source archive checklist. Keep these optional if they add too much maintenance.

8. Review CI.
   Keep lint, build matrix, FreeBSD, tests, and sanitizers. Add any new pure tests
   and benchmark smoke tests. Ensure clang-format version is documented.

9. Audit docs.
   Update `.docs/HANDOFF.md` to reflect the implemented state and remove stale
   roadmap text. Keep `.docs/refs/` gitignored.

10. Do a final product pass.
    Try paige on real logs, long lines, colored output, small files, empty files,
    piped stdin, resized terminals, and no-controlling-terminal cases.

11. Review performance claims.
    Every README or release claim about speed should point to a benchmark or be
    softened to a design goal.

12. Review innovation features for fallback behavior.
    Progressive status, semantic jumps, filters, minimaps, and perf overlays should
    degrade cleanly when hosts do not provide optional metadata.

## Pitfalls To Avoid

1. Do not ship hidden experimental API fields without comments.

2. Do not make the release process depend on locally cloned `.docs/refs/`.

3. Do not add packaging complexity that makes the simple `make` workflow worse.

4. Do not ignore terminal cleanup just because tests pass.

5. Do not claim Unicode or ANSI behavior that is not actually tested.

6. Do not let docs describe mat-specific behavior as core paige behavior.

7. Do not ship unmeasured speed claims.

8. Do not let optional innovation features make the simple pager path slower.

## Definition Of Done

1. `README.md` is accurate for a new standalone user.

2. `include/paige.h` is release-reviewed and documented.

3. `make`, `make test`, formatter check, CI matrix, and sanitizer build pass.

4. Release limitations are documented honestly.

5. Benchmarks have at least one recorded baseline.

6. `.docs/HANDOFF.md` is updated as the next pickup point.

7. The repo can be cloned without `.docs/refs/` and still contains all tracked
   planning, audit, build, test, and user documentation needed to continue.

8. Performance and innovation features have documented limitations and fallback
   paths.

## General Guidelines

1. Polish by removing surprises, not by adding features.

2. Keep the library easier to embed than a full pager program.

3. Be conservative with public promises.

4. Prefer explicit unsupported-status behavior over silent degradation.
