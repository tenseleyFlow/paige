/*
 * paige-demo — a minimal standalone pager built on the paige engine.
 *
 * Opens a file (mmap) or drains a stream (slurp), then pages it, building the
 * newline-offset index ONLY as far as the viewport asks — so first paint is
 * O(screen) regardless of file size, the same lazy contract real hosts (e.g.
 * mat's linesrc) use. It exists to exercise and demonstrate the library.
 *
 * Because it mmaps regular files, a file truncated by another process while
 * mapped would fault on the vanished pages; a SIGBUS handler with siglongjmp
 * recovers (degrading to a short read) instead of crashing — see linesrc.c in
 * the mat tree for the production version of this pattern.
 */
#include "paige.h"

#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct doc {
    const char *data; /* logical bytes: mmap base or slurp buffer */
    size_t size;
    void *map;       /* mmap base to munmap, or NULL when slurped */
    size_t map_size; /* length passed to munmap */
    char *owned;     /* malloc'd slurp buffer to free, or NULL when mmap'd */
    size_t *line;    /* byte offset of each logical line discovered so far */
    size_t nlines;   /* offsets KNOWN so far (not the document total) */
    size_t line_cap;
    int eof_known; /* set once the index has reached EOF */
    size_t total;  /* logical line count; valid only when eof_known */
    int ends_nl;   /* whether the file ends with '\n' (valid when eof_known) */
    char *rawbuf;  /* scratch for raw_line copies out of mmap */
    size_t rawcap;
    const char *path;
    const char *title;
    char *hlbuf; /* reused highlight scratch, grown as needed */
    size_t hlcap;
};

/*
 * SIGBUS recovery for mmap'd files truncated underneath us. The handler is a
 * no-op unless armed; each function that reads mmap'd bytes arms a fresh
 * sigsetjmp target right before its reads, so a fault unwinds locally and the
 * read is reported as a short/empty result rather than killing the process.
 */
static _Thread_local sigjmp_buf sigbus_jmp;
static _Thread_local volatile sig_atomic_t sigbus_armed;

static void on_sigbus(int sig)
{
    (void)sig;
    if (sigbus_armed)
        siglongjmp(sigbus_jmp, 1);
    _exit(128 + SIGBUS);
}

static void install_sigbus(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigbus;
    sigaction(SIGBUS, &sa, NULL);
}

static void off_push(struct doc *d, size_t v)
{
    if (d->nlines == d->line_cap) {
        size_t nc = d->line_cap ? d->line_cap * 2 : 256;
        size_t *nb = realloc(d->line, nc * sizeof *nb);
        if (!nb) { /* out of memory: stop indexing, treat here as EOF */
            d->eof_known = 1;
            d->total = d->nlines > 0 ? d->nlines - 1 : 0;
            return;
        }
        d->line = nb;
        d->line_cap = nc;
    }
    d->line[d->nlines++] = v;
}

/* Grow the offset table to include line `want` (or to EOF), scanning forward
 * from the last known offset with memchr. The whole scan runs under one SIGBUS
 * guard since it is the heaviest mmap read. */
static void ensure(struct doc *d, size_t want)
{
    if (d->eof_known || d->nlines > want)
        return;
    if (d->map && sigsetjmp(sigbus_jmp, 1) != 0) {
        d->eof_known = 1; /* truncated mid-scan: stop where we got to */
        d->total = d->nlines > 0 ? d->nlines - 1 : 0;
        return;
    }
    while (!d->eof_known && d->nlines <= want) {
        size_t from = d->line[d->nlines - 1];
        if (from >= d->size) {
            d->eof_known = 1;
            d->total = d->nlines - 1;
            break;
        }
        const char *nl = memchr(d->data + from, '\n', d->size - from);
        if (!nl) { /* final line has no trailing newline */
            d->eof_known = 1;
            d->total = d->nlines;
            break;
        }
        size_t off = (size_t)(nl - d->data) + 1;
        if (off < d->size) {
            off_push(d, off);
        } else { /* newline is the very last byte: that was the last line */
            d->eof_known = 1;
            d->total = d->nlines;
            break;
        }
    }
    if (d->eof_known)
        d->ends_nl = (d->size > 0 && d->data[d->size - 1] == '\n');
}

/* Resolve the byte range of logical line L. Reads only the heap offset table
 * and the ends_nl flag — never mmap content — so it needs no SIGBUS guard. */
static int line_bounds(struct doc *d, size_t L, size_t *start, size_t *len)
{
    ensure(d, L + 1);
    if (d->eof_known && L >= d->total)
        return 0;
    if (L >= d->nlines)
        return 0;
    size_t s = d->line[L];
    size_t e;
    if (L + 1 < d->nlines) {
        e = d->line[L + 1] - 1; /* the byte before line L+1 is the newline */
    } else {
        e = d->size; /* last known line runs to EOF */
        if (d->ends_nl && e > s)
            e--;
    }
    *start = s;
    *len = e > s ? e - s : 0;
    return 1;
}

/* Emit one logical line wrapped to `width`, reading mmap bytes under a SIGBUS
 * guard. Returns the segment count, or -1 if the mapping faulted (truncated).
 * The guard lives in its own function so no caller local is in its setjmp scope
 * (gcc -Wclobbered). */
static int emit_wrapped(struct doc *d, paige_sink *sink, size_t start,
                        size_t len, int width)
{
    if (d->map && sigsetjmp(sigbus_jmp, 1) != 0)
        return -1;
    int segs = 0;
    for (size_t i = 0; i < len; i += (size_t)width) {
        size_t chunk = (len - i < (size_t)width) ? len - i : (size_t)width;
        paige_emit(sink, d->data + start + i, chunk);
        segs++;
    }
    return segs;
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
    int segs = emit_wrapped(d, sink, start, len, width);
    return segs < 0 ? 0 : segs; /* faulted: truncate the frame, stay alive */
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

/* Emit the requested segment window of one line, reading mmap bytes under a
 * SIGBUS guard (isolated so no caller local is in its setjmp scope). Returns
 * the line's total segment count, or -1 if the mapping faulted. */
static int render_ex_emit(struct doc *d, paige_sink *sink,
                          const paige_render_req *req, size_t start, size_t len,
                          int width, const paige_match *matches,
                          size_t nmatches)
{
    if (d->map && sigsetjmp(sigbus_jmp, 1) != 0)
        return -1;

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

    int total =
        render_ex_emit(d, sink, req, start, len, width, matches, nmatches);
    return total < 0 ? 0 : total; /* faulted: truncate the frame, stay alive */
}

/* Copy a line's bytes out of (possibly mmap'd) storage under a SIGBUS guard.
 * Returns 0 on success, -1 if the mapping faulted. */
static int copy_guarded(struct doc *d, size_t start, size_t len)
{
    if (sigsetjmp(sigbus_jmp, 1) != 0)
        return -1;
    memcpy(d->rawbuf, d->data + start, len);
    return 0;
}

static int raw_line(void *ctx, size_t L, paige_line *out)
{
    struct doc *d = ctx;
    size_t start, len;
    if (!line_bounds(d, L, &start, &len))
        return 0;
    if (!d->map) { /* slurped: the heap buffer is stable, hand it back direct */
        out->bytes = d->data + start;
        out->len = len;
        return 1;
    }
    /* mmap'd: copy out under a SIGBUS guard so callers (e.g. paige's search)
     * never read a page that may vanish under them. */
    if (d->rawcap < len) {
        char *nb = realloc(d->rawbuf, len ? len : 1);
        if (!nb)
            return 0;
        d->rawbuf = nb;
        d->rawcap = len ? len : 1;
    }
    if (copy_guarded(d, start, len) != 0)
        return 0;
    out->bytes = d->rawbuf;
    out->len = len;
    return 1;
}

static int line_count(void *ctx, size_t *out)
{
    struct doc *d = ctx;
    ensure(d, (size_t)-1); /* lazy: scan to EOF only when % first asks */
    *out = d->total;
    return 1;
}

/* Reference seek_end: name the last line so `G` is O(screen) without a
 * line_count. This demo must index to EOF to learn the last index; a host that
 * can seek by byte offset (or already tracks a lazy total) answers cheaper. */
static int seek_end(void *ctx, size_t *out)
{
    struct doc *d = ctx;
    ensure(d, (size_t)-1);
    if (d->total == 0)
        return 0;
    *out = d->total - 1;
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
        /* Scan forward, letting line_bounds extend the index lazily, until a
         * landmark is found or EOF (line_bounds returns 0). */
        for (size_t L = from + 1;; L++) {
            size_t s, len;
            if (!line_bounds(d, L, &s, &len))
                break;
            if (is_landmark(d, L)) {
                *out = L;
                return 1;
            }
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

static char *slurp_fd(int fd, size_t *out_size)
{
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;
    for (;;) {
        if (len == cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) {
            free(buf);
            return NULL;
        }
        if (r == 0)
            break;
        len += (size_t)r;
    }
    *out_size = len;
    return buf;
}

/* Open `path` (NULL = stdin): mmap regular files for O(screen) first paint,
 * slurp pipes / non-seekable / mmap failures. Seeds the lazy index at line 0.
 */
static int open_doc(struct doc *d, const char *path)
{
    int fd = path ? open(path, O_RDONLY) : STDIN_FILENO;
    if (fd < 0)
        return 0;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m != MAP_FAILED) {
            d->map = m;
            d->map_size = (size_t)st.st_size;
            d->data = m;
            d->size = (size_t)st.st_size;
            sigbus_armed = 1; /* mmap is live for the rest of the run */
            if (path)
                close(fd); /* the mapping stays valid after close */
            off_push(d, 0);
            return 1;
        }
    }
    /* pipe / stdin / non-seekable / empty / mmap failed: read it into memory */
    size_t size = 0;
    char *buf = slurp_fd(fd, &size);
    if (path && fd >= 0)
        close(fd);
    if (!buf)
        return 0;
    d->owned = buf;
    d->data = buf;
    d->size = size;
    off_push(d, 0);
    return 1;
}

static void close_doc(struct doc *d)
{
    if (d->map)
        munmap(d->map, d->map_size);
    free(d->owned);
    free(d->line);
    free(d->rawbuf);
    free(d->hlbuf);
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

    /* Content grew/changed: drop the old mapping and re-open from scratch. */
    struct doc nd;
    memset(&nd, 0, sizeof nd);
    nd.path = d->path;
    nd.title = d->title;
    if (!open_doc(&nd, d->path))
        return 0;
    if (d->map)
        munmap(d->map, d->map_size);
    free(d->owned);
    free(d->line);
    /* keep rawbuf/hlbuf scratch across refreshes */
    nd.rawbuf = d->rawbuf;
    nd.rawcap = d->rawcap;
    nd.hlbuf = d->hlbuf;
    nd.hlcap = d->hlcap;
    *d = nd;
    return 1;
}

/* Open `path` (NULL = stdin) and fill a paige_doc for it. */
static int fill_doc(struct doc *d, paige_doc *pd, const char *path)
{
    memset(d, 0, sizeof *d);
    d->path = path;
    d->title = path ? path : "stdin";
    if (!open_doc(d, path))
        return 0;
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

/* Write the whole document to stdout under a SIGBUS guard (the no-terminal
 * fallback). Isolated so main's locals are not in the setjmp scope. */
static void dump_plain(struct doc *d)
{
    if (d->map && sigsetjmp(sigbus_jmp, 1) != 0)
        return;
    (void)!write(STDOUT_FILENO, d->data, d->size);
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

    install_sigbus();

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
        dump_plain(&d[0]);
    }
    if (show_stats) {
        fprintf(
            stderr,
            "paige-stats: render=%llu frames=%llu rows=%llu bytes=%llu "
            "writes=%llu search_lines=%llu hscroll=%llu follow_refreshes=%llu "
            "follow_updates=%llu segments=%llu host_indexed=%zu\n",
            stats.render_calls, stats.frames, stats.rows_drawn,
            stats.bytes_emitted, stats.writes, stats.search_lines,
            stats.hscroll_moves, stats.follow_refreshes, stats.follow_updates,
            stats.segments_emitted, d[0].nlines);
    }
    for (size_t i = 0; i < ndocs; i++)
        close_doc(&d[i]);
    free(d);
    free(docs);
    return 0;
}
