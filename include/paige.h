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

typedef struct paige_line {
    const char *bytes;
    size_t len;
} paige_line;

typedef struct paige_match {
    size_t off; /* byte offset within the raw logical line */
    size_t len;
    int active;
} paige_match;

enum {
    PAIGE_RENDER_WRAP = 0,
    PAIGE_RENDER_CHOP = 1u << 0,
};

typedef struct paige_render_req {
    size_t lineno;
    int width;
    unsigned flags;
    size_t hscroll;
    const paige_match *matches;
    size_t nmatches;
} paige_render_req;

typedef struct paige_stats {
    unsigned long long render_calls;
    unsigned long long frames;
    unsigned long long rows_drawn;
    unsigned long long bytes_emitted;
    unsigned long long writes;
    unsigned long long search_lines;
} paige_stats;

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

    /*
     * Optional raw logical line access for features that must not search or
     * reason over terminal-rendered output. Return 1 when `lineno` exists and
     * fill `out`; return 0 at EOF. Bytes must remain valid until the next call
     * into the same document context.
     */
    int (*raw_line)(void *ctx, size_t lineno, paige_line *out);

    /*
     * Optional extended renderer. When set, paige calls this instead of
     * render_line(). The request carries future-proof draw context such as wrap
     * mode, horizontal offset, and match spans. Return the emitted segment count
     * or 0 at EOF, with the same determinism requirements as render_line().
     */
    int (*render_line_ex)(void *ctx, const paige_render_req *req,
                          paige_sink *sink);
} paige_doc;

typedef struct paige_opts {
    int quit_if_one_screen; /* if nonzero, just print and return when it fits */
    int goto_pause_ms;      /* digit-goto entry timeout; <=0 uses the default */
    paige_stats *stats;     /* optional counters; zeroed at paige_run start */
} paige_opts;

/*
 * Page the document interactively. Returns 0 on a normal quit, or -1 if there
 * is no usable controlling terminal (the caller should then print plainly).
 */
int paige_run(const paige_doc *doc, const paige_opts *opts);

#endif /* PAIGE_H */
