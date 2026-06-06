/*
 * paige-demo — a minimal standalone pager built on the paige engine.
 *
 * Reads a file (or stdin) into memory, indexes lines, and pages with simple
 * byte-based wrapping. It exists to exercise and demonstrate the library; real
 * clients (e.g. mat) supply their own width-aware render_line.
 */
#include "paige.h"

#include <fcntl.h>
#include <limits.h>
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
    char *hlbuf; /* reused highlight scratch, grown as needed */
    size_t hlcap;
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

static void emit_highlighted(struct doc *d, paige_sink *sink, const char *bytes,
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

    /* Reuse a per-document scratch buffer instead of malloc/free per segment
     * per row per frame while a search is active. */
    size_t cap = len + overlaps * SGR_LEN * 2;
    if (d->hlcap < cap) {
        char *nb = realloc(d->hlbuf, cap);
        if (!nb) {
            paige_emit(sink, bytes, len); /* degrade: emit unhighlighted */
            return;
        }
        d->hlbuf = nb;
        d->hlcap = cap;
    }
    char *out = d->hlbuf;

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
}

/* Emit one segment wrapped in bold, to highlight a freshly-appended line. */
static void emit_appended(struct doc *d, paige_sink *sink, const char *bytes,
                          size_t len)
{
    size_t need = len + 8; /* \x1b[1m (4) + \x1b[0m (4) */
    if (d->hlcap < need) {
        char *nb = realloc(d->hlbuf, need);
        if (!nb) {
            paige_emit(sink, bytes, len);
            return;
        }
        d->hlbuf = nb;
        d->hlcap = need;
    }
    memcpy(d->hlbuf, "\x1b[1m", 4);
    memcpy(d->hlbuf + 4, bytes, len);
    memcpy(d->hlbuf + 4 + len, "\x1b[0m", 4);
    paige_emit(sink, d->hlbuf, len + 8);
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
            if (req->appended)
                emit_appended(d, sink, d->data + start + off, chunk);
            else
                emit_highlighted(d, sink, d->data + start + off, off, chunk,
                                 matches, nmatches);
        }
        return 1;
    }

    size_t total = (len + (size_t)width - 1) / (size_t)width;
    if (total > (size_t)INT_MAX)
        total =
            (size_t)INT_MAX; /* render_line returns int: clamp absurd lines */
    size_t emitted = 0;
    for (size_t s = req->seg_first; s < total && emitted < req->seg_max; s++) {
        size_t i = s * (size_t)width;
        size_t chunk = (len - i < (size_t)width) ? len - i : (size_t)width;
        if (req->appended)
            emit_appended(d, sink, d->data + start + i, chunk);
        else
            emit_highlighted(d, sink, d->data + start + i, i, chunk, matches,
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

/* Reference seek_end: hand the engine the last line index so `G` is O(screen)
 * without a line_count. This demo already holds a full index, so it is trivial;
 * a lazily-indexing host (e.g. mat) would seek/scan from EOF to find it. */
static int seek_end(void *ctx, size_t *out)
{
    struct doc *d = ctx;
    if (d->nlines == 0)
        return 0;
    *out = d->nlines - 1;
    return 1;
}

/* A landmark is a structural marker a reader jumps between: a header (#) or an
 * ERROR/WARN log line. A real host would recognize its own semantics. */
static int is_landmark(struct doc *d, size_t L)
{
    size_t s, len;
    if (!line_bounds(d, L, &s, &len))
        return 0;
    const char *p = d->data + s;
    return (len >= 1 && p[0] == '#') ||
           (len >= 5 && memcmp(p, "ERROR", 5) == 0) ||
           (len >= 4 && memcmp(p, "WARN", 4) == 0);
}

static int landmark(void *ctx, size_t from, int dir, size_t *out)
{
    struct doc *d = ctx;
    if (dir > 0) {
        for (size_t L = from + 1; L < d->nlines; L++)
            if (is_landmark(d, L)) {
                *out = L;
                return 1;
            }
    } else {
        for (size_t L = from; L-- > 0;)
            if (is_landmark(d, L)) {
                *out = L;
                return 1;
            }
    }
    return 0;
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

/* Slurp + index `path` (NULL = stdin) and fill a paige_doc for it. */
static int fill_doc(struct doc *d, paige_doc *pd, const char *path)
{
    memset(d, 0, sizeof *d);
    d->data = slurp(path, &d->size);
    if (!d->data)
        return 0;
    d->title = path ? path : "stdin";
    d->path = path;
    index_lines(d);
    *pd = (paige_doc){.ctx = d,
                      .render_line = render_line,
                      .title = d->title,
                      .raw_line = raw_line,
                      .line_count = line_count,
                      .seek_end = seek_end,
                      .render_line_ex = render_line_ex,
                      .refresh = refresh_doc,
                      .landmark = landmark};
    if (getenv("PAIGE_NO_RAW"))
        pd->raw_line = NULL;
    if (getenv("PAIGE_NO_COUNT"))
        pd->line_count = NULL;
    if (getenv("PAIGE_NO_SEEK_END"))
        pd->seek_end = NULL;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 &&
        (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("paige-demo (paige %d.%d.%d)\n", PAIGE_VERSION_MAJOR,
               PAIGE_VERSION_MINOR, PAIGE_VERSION_PATCH);
        return 0;
    }
    if (argc < 2 && isatty(STDIN_FILENO)) {
        fprintf(stderr,
                "usage: paige-demo FILE... (or pipe content on stdin)\n");
        return 2;
    }

    size_t ndocs = argc > 1 ? (size_t)(argc - 1) : 1; /* stdin counts as 1 */
    struct doc *d = calloc(ndocs, sizeof *d);
    paige_doc *docs = calloc(ndocs, sizeof *docs);
    if (!d || !docs) {
        perror("calloc");
        return 1;
    }
    for (size_t i = 0; i < ndocs; i++) {
        const char *path = argc > 1 ? argv[i + 1] : NULL;
        if (!fill_doc(&d[i], &docs[i], path)) {
            fprintf(stderr, "paige-demo: cannot read %s\n",
                    path ? path : "stdin");
            return 1;
        }
    }

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
    if (paige_run_many(docs, ndocs, &opts) < 0) {
        /* No terminal: dump the first document plainly. */
        (void)!write(STDOUT_FILENO, d[0].data, d[0].size);
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
    for (size_t i = 0; i < ndocs; i++) {
        free(d[i].data);
        free(d[i].line);
        free(d[i].hlbuf);
    }
    free(d);
    free(docs);
    return 0;
}
