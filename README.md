# paige

A small, fast, dependency-free terminal **pager engine** in C — the bespoke
pager behind [`mat`](https://github.com/tenseleyFlow/mat), built so it can be
reused on its own.

paige owns the terminal (raw mode, alternate screen, scrolling, keys, resize,
status line). It does **not** own the content: the client supplies a callback
that renders one logical line into visual segments for a given width. paige only
ever calls it for the lines it needs to show, so paging a multi-gigabyte file
never renders more than a screenful — it's lazy by construction.

Keyboard input is read from `/dev/tty`, so the content can be piped in.

## API

```c
#include <paige.h>

int my_render(void *ctx, size_t lineno, int width, paige_sink *sink) {
    // emit the visual segments of logical line `lineno` (wrapped to `width`)
    // via paige_emit(); return the segment count, or 0 at end-of-document.
}

paige_doc doc  = { .ctx = ..., .render_line = my_render, .title = "file" };
paige_opts opts = { .quit_if_one_screen = 1 };
paige_run(&doc, &opts);   // 0 on quit, -1 if there's no terminal
```

## Build

```sh
make            # builds build/libpaige.a and the paige-demo binary
make test       # PTY-driven interactive tests
./paige-demo FILE   # a minimal standalone pager
```

No external dependencies. C11 + POSIX (termios), portable across Linux, macOS,
and the BSDs.

## Keys
`q` quit · `j`/`k`/`↑`/`↓` line · space/`f`/`b` page · `d`/`u` half-page ·
`g`/`G` top/bottom · digits jump to a line as you type — the view follows each
keystroke (`1`,`6` → line 16), and a pause longer than ~600ms commits the number
and starts a fresh one (`1` … `6` → line 6).

## License
MIT — see [LICENSE](LICENSE).
