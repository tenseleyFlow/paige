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
#include <sys/stat.h>
#include <unistd.h>

struct doc {
    char *data;
    size_t size;
    size_t *line; /* byte offset of each logical line */
    size_t nlines;
    const char *path;
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

static int match_cmp(const void *a, const void *b)
{
    const paige_match *ma = a;
    const paige_match *mb = b;
    if (ma->off < mb->off)
        return -1;
    if (ma->off > mb->off)
        return 1;
    return 0;
}

static void emit_highlighted(paige_sink *sink, const char *bytes,
                             size_t seg_start, size_t len,
                             const paige_match *matches, size_t nmatches)
{
    enum { SGR_LEN = 4 };
    const char *on = "\x1b[7m";
    const char *off = "\x1b[0m";
    size_t overlaps = 0;
    for (size_t i = 0; i < nmatches; i++) {
        size_t m0 = matches[i].off;
        size_t m1 = matches[i].off + matches[i].len;
        if (m1 > seg_start && m0 < seg_start + len)
            overlaps++;
    }
    if (overlaps == 0) {
        paige_emit(sink, bytes, len);
        return;
    }

    size_t cap = len + overlaps * SGR_LEN * 2;
    char *out = malloc(cap);
    if (!out) {
        paige_emit(sink, bytes, len);
        return;
    }

    size_t pos = 0, out_len = 0;
    for (size_t i = 0; i < nmatches && pos < len; i++) {
        size_t m0 = matches[i].off;
        size_t m1 = matches[i].off + matches[i].len;
        if (m1 <= seg_start || m0 >= seg_start + len)
            continue;
        size_t a = m0 > seg_start ? m0 - seg_start : 0;
        size_t b = m1 < seg_start + len ? m1 - seg_start : len;
        if (a > pos) {
            memcpy(out + out_len, bytes + pos, a - pos);
            out_len += a - pos;
        }
        memcpy(out + out_len, on, SGR_LEN);
        out_len += SGR_LEN;
        memcpy(out + out_len, bytes + a, b - a);
        out_len += b - a;
        memcpy(out + out_len, off, SGR_LEN);
        out_len += SGR_LEN;
        pos = b;
    }
    if (pos < len) {
        memcpy(out + out_len, bytes + pos, len - pos);
        out_len += len - pos;
    }
    paige_emit(sink, out, out_len);
    free(out);
}

static int render_line_ex(void *ctx, const paige_render_req *req,
                          paige_sink *sink)
{
    struct doc *d = ctx;
    size_t start, len;
    if (!line_bounds(d, req->lineno, &start, &len))
        return 0;
    int width = req->width;
    if (width < 1)
        width = 1;
    /* req->seg_max == 0 means "count only, emit nothing"; otherwise emit just
     * the requested window [seg_first, seg_first+seg_max). Total segment count
     * is always returned so the pager can scroll. */
    if (len == 0) {
        if (req->seg_max != 0 && req->seg_first == 0)
            paige_emit(sink, "", 0);
        return 1;
    }

    paige_match matches[64];
    size_t nmatches = req->nmatches < 64 ? req->nmatches : 64;
    if (nmatches > 0) {
        memcpy(matches, req->matches, nmatches * sizeof *matches);
        qsort(matches, nmatches, sizeof *matches, match_cmp);
    }

    if ((req->flags & PAIGE_RENDER_CHOP) != 0) {
        if (req->seg_max != 0) {
            size_t off = req->hscroll < len ? req->hscroll : len;
            size_t chunk =
                len - off < (size_t)width ? len - off : (size_t)width;
            emit_highlighted(sink, d->data + start + off, off, chunk, matches,
                             nmatches);
        }
        return 1;
    }

    size_t total = (len + (size_t)width - 1) / (size_t)width;
    size_t emitted = 0;
    for (size_t s = req->seg_first; s < total && emitted < req->seg_max; s++) {
        size_t i = s * (size_t)width;
        size_t chunk = (len - i < (size_t)width) ? len - i : (size_t)width;
        emit_highlighted(sink, d->data + start + i, i, chunk, matches,
                         nmatches);
        emitted++;
    }
    return (int)total;
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

static int line_count(void *ctx, size_t *out)
{
    struct doc *d = ctx;
    *out = d->nlines;
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

static int refresh_doc(void *ctx)
{
    struct doc *d = ctx;
    if (!d->path)
        return 0;
    struct stat st;
    if (stat(d->path, &st) < 0 || st.st_size < 0)
        return 0;
    if ((size_t)st.st_size == d->size)
        return 0;

    size_t size = 0;
    char *data = slurp(d->path, &size);
    if (!data)
        return 0;
    free(d->data);
    free(d->line);
    d->data = data;
    d->size = size;
    d->line = NULL;
    d->nlines = 0;
    index_lines(d);
    return 1;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : NULL;
    if (path && (strcmp(path, "--version") == 0 || strcmp(path, "-V") == 0)) {
        printf("paige-demo (paige %d.%d.%d)\n", PAIGE_VERSION_MAJOR,
               PAIGE_VERSION_MINOR, PAIGE_VERSION_PATCH);
        return 0;
    }
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
    d.path = path;
    index_lines(&d);

    paige_doc doc = {.ctx = &d,
                     .render_line = render_line,
                     .title = d.title,
                     .raw_line = raw_line,
                     .line_count = line_count,
                     .render_line_ex = render_line_ex,
                     .refresh = refresh_doc};
    if (getenv("PAIGE_NO_RAW"))
        doc.raw_line = NULL;
    if (getenv("PAIGE_NO_COUNT"))
        doc.line_count = NULL;
    paige_stats stats = {0};
    paige_opts opts = {.quit_if_one_screen = 1};
    if (getenv("PAIGE_CHOP"))
        opts.chop_long_lines = 1;
    const char *show_stats = getenv("PAIGE_STATS");
    if (show_stats)
        opts.stats = &stats;
    /* PAIGE_GOTO_MS lets the PTY test tune the digit-goto timeout. */
    const char *gms = getenv("PAIGE_GOTO_MS");
    if (gms)
        opts.goto_pause_ms = atoi(gms);
    const char *fms = getenv("PAIGE_FOLLOW_MS");
    if (fms)
        opts.follow_poll_ms = atoi(fms);
    if (paige_run(&doc, &opts) < 0) {
        /* No terminal: dump plainly. */
        (void)!write(STDOUT_FILENO, d.data, d.size);
    }
    if (show_stats) {
        fprintf(
            stderr,
            "paige-stats: render=%llu frames=%llu rows=%llu bytes=%llu "
            "writes=%llu search_lines=%llu hscroll=%llu follow_refreshes=%llu "
            "follow_updates=%llu segments=%llu\n",
            stats.render_calls, stats.frames, stats.rows_drawn,
            stats.bytes_emitted, stats.writes, stats.search_lines,
            stats.hscroll_moves, stats.follow_refreshes, stats.follow_updates,
            stats.segments_emitted);
    }
    free(d.data);
    free(d.line);
    return 0;
}
