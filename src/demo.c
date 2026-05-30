/*
 * paige-demo — a minimal standalone pager built on the paige engine.
 *
 * Reads a file (or stdin) into memory, indexes lines, and pages with simple
 * byte-based wrapping. It exists to exercise and demonstrate the library; real
 * clients (e.g. mat) supply their own width-aware render_line.
 */
#include "paige.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct doc {
    char *data;
    size_t size;
    size_t *line; /* byte offset of each logical line */
    size_t nlines;
    const char *title;
};

static int line_bounds(struct doc *d, size_t L, size_t *start, size_t *len)
{
    if (L >= d->nlines)
        return 0;
    size_t s = d->line[L];
    size_t e = (L + 1 < d->nlines) ? d->line[L + 1] : d->size;
    if (e > s && d->data[e - 1] == '\n')
        e--;
    *start = s;
    *len = e - s;
    return 1;
}

static int render_line(void *ctx, size_t L, int width, paige_sink *sink)
{
    struct doc *d = ctx;
    size_t start, len;
    if (!line_bounds(d, L, &start, &len))
        return 0;
    if (width < 1)
        width = 1;
    if (len == 0) {
        paige_emit(sink, "", 0);
        return 1;
    }
    int segs = 0;
    for (size_t i = 0; i < len; i += (size_t)width) {
        size_t chunk = (len - i < (size_t)width) ? len - i : (size_t)width;
        paige_emit(sink, d->data + start + i, chunk);
        segs++;
    }
    return segs;
}

static int render_line_ex(void *ctx, const paige_render_req *req,
                          paige_sink *sink)
{
    (void)req->flags;
    (void)req->hscroll;
    (void)req->matches;
    (void)req->nmatches;
    return render_line(ctx, req->lineno, req->width, sink);
}

static int raw_line(void *ctx, size_t L, paige_line *out)
{
    struct doc *d = ctx;
    size_t start, len;
    if (!line_bounds(d, L, &start, &len))
        return 0;
    out->bytes = d->data + start;
    out->len = len;
    return 1;
}

static char *slurp(const char *path, size_t *out_size)
{
    int fd = path ? open(path, O_RDONLY) : STDIN_FILENO;
    if (fd < 0)
        return NULL;
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) {
            free(buf);
            if (path)
                close(fd);
            return NULL;
        }
        if (r == 0)
            break;
        len += (size_t)r;
    }
    if (path)
        close(fd);
    *out_size = len;
    return buf;
}

static void index_lines(struct doc *d)
{
    size_t cap = 256;
    d->line = malloc(cap * sizeof *d->line);
    d->nlines = 0;
    d->line[d->nlines++] = 0;
    for (size_t i = 0; i < d->size; i++) {
        if (d->data[i] == '\n' && i + 1 <= d->size) {
            if (d->nlines == cap) {
                cap *= 2;
                d->line = realloc(d->line, cap * sizeof *d->line);
            }
            if (i + 1 < d->size)
                d->line[d->nlines++] = i + 1;
        }
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : NULL;
    if (path == NULL && isatty(STDIN_FILENO)) {
        fprintf(stderr, "usage: paige-demo FILE (or pipe content on stdin)\n");
        return 2;
    }
    struct doc d;
    memset(&d, 0, sizeof d);
    d.data = slurp(path, &d.size);
    if (d.data == NULL) {
        fprintf(stderr, "paige-demo: cannot read %s\n", path ? path : "stdin");
        return 1;
    }
    d.title = path ? path : "stdin";
    index_lines(&d);

    paige_doc doc = { .ctx = &d,
                      .render_line = render_line,
                      .title = d.title,
                      .raw_line = raw_line,
                      .render_line_ex = render_line_ex };
    paige_stats stats = {0};
    paige_opts opts = { .quit_if_one_screen = 1 };
    const char *show_stats = getenv("PAIGE_STATS");
    if (show_stats)
        opts.stats = &stats;
    /* PAIGE_GOTO_MS lets the PTY test tune the digit-goto timeout. */
    const char *gms = getenv("PAIGE_GOTO_MS");
    if (gms)
        opts.goto_pause_ms = atoi(gms);
    if (paige_run(&doc, &opts) < 0) {
        /* No terminal: dump plainly. */
        (void)!write(STDOUT_FILENO, d.data, d.size);
    }
    if (show_stats) {
        fprintf(stderr,
                "paige-stats: render=%llu frames=%llu rows=%llu bytes=%llu "
                "writes=%llu\n",
                stats.render_calls, stats.frames, stats.rows_drawn,
                stats.bytes_emitted, stats.writes);
    }
    free(d.data);
    free(d.line);
    return 0;
}
