/*
 * paige.h — a small, fast, dependency-free terminal pager engine.
 *
 * paige owns the terminal: raw mode, the alternate screen, scrolling, key
 * handling, resize, and the status line. It does NOT own the content. The
 * client supplies a callback that renders one logical line into visual segments
 * for a given width; paige only ever calls it for the lines it needs to show,
 * so paging a multi-gigabyte file never renders more than a screenful.
 *
 * Reading keys from /dev/tty (not stdin) means the content may be piped in.
 */
#ifndef PAIGE_H
#define PAIGE_H

#include <stddef.h>

/* Opaque sink: the client calls paige_emit() once per visual segment (one
 * screen row's worth of bytes, ANSI allowed, no trailing newline). */
typedef struct paige_sink paige_sink;
void paige_emit(paige_sink *sink, const char *bytes, size_t len);

typedef struct paige_doc {
    void *ctx;

    /*
     * Emit the visual segments of logical line `lineno`, laid out for a content
     * area of `width` columns, by calling paige_emit() once per segment in top
     * to bottom order. Return the number of segments emitted (>= 1), or 0 to
     * signal that `lineno` is at or past the end of the document.
     *
     * Must be deterministic for a given (lineno, width): paige may call it more
     * than once for the same line (e.g. on redraw or scroll).
     */
    int (*render_line)(void *ctx, size_t lineno, int width, paige_sink *sink);

    const char
        *title; /* shown in the status line, e.g. a filename (may be NULL) */
} paige_doc;

typedef struct paige_opts {
    int quit_if_one_screen; /* if nonzero, just print and return when it fits */
} paige_opts;

/*
 * Page the document interactively. Returns 0 on a normal quit, or -1 if there
 * is no usable controlling terminal (the caller should then print plainly).
 */
int paige_run(const paige_doc *doc, const paige_opts *opts);

#endif /* PAIGE_H */
