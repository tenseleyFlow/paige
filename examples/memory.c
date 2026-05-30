/* A tiny host-owned document example: raw search plus chop-mode rendering. */
#include "paige.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct mem_doc {
    const char *const *lines;
    size_t nlines;
};

static int bounds(struct mem_doc *d, size_t L, const char **line, size_t *len)
{
    if (L >= d->nlines)
        return 0;
    *line = d->lines[L];
    *len = strlen(*line);
    return 1;
}

static int emit_wrap(const char *line, size_t len, int width, paige_sink *sink)
{
    if (width < 1)
        width = 1;
    if (len == 0) {
        paige_emit(sink, "", 0);
        return 1;
    }
    int segs = 0;
    for (size_t i = 0; i < len; i += (size_t)width) {
        size_t chunk = len - i < (size_t)width ? len - i : (size_t)width;
        paige_emit(sink, line + i, chunk);
        segs++;
    }
    return segs;
}

static int render_line(void *ctx, size_t L, int width, paige_sink *sink)
{
    struct mem_doc *d = ctx;
    const char *line;
    size_t len;
    if (!bounds(d, L, &line, &len))
        return 0;
    return emit_wrap(line, len, width, sink);
}

static int render_line_ex(void *ctx, const paige_render_req *req,
                          paige_sink *sink)
{
    struct mem_doc *d = ctx;
    const char *line;
    size_t len;
    if (!bounds(d, req->lineno, &line, &len))
        return 0;
    if ((req->flags & PAIGE_RENDER_CHOP) == 0)
        return emit_wrap(line, len, req->width, sink);
    int width = req->width < 1 ? 1 : req->width;
    size_t off = req->hscroll < len ? req->hscroll : len;
    size_t chunk = len - off < (size_t)width ? len - off : (size_t)width;
    paige_emit(sink, line + off, chunk);
    return 1;
}

static int raw_line(void *ctx, size_t L, paige_line *out)
{
    struct mem_doc *d = ctx;
    const char *line;
    size_t len;
    if (!bounds(d, L, &line, &len))
        return 0;
    out->bytes = line;
    out->len = len;
    return 1;
}

static int line_count(void *ctx, size_t *out)
{
    struct mem_doc *d = ctx;
    *out = d->nlines;
    return 1;
}

int main(void)
{
    static const char *const lines[] = {
        "paige memory example",
        "This document is owned entirely by the host program.",
        "raw_line enables /search and ?search over source bytes.",
        "line_count enables 50% style jumps without scanning to EOF.",
        "render_line_ex enables chop mode and horizontal scrolling.",
        ("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz "
         "-- long line for chop mode"),
        "Press /raw, 50%, left/right arrows, h, P, or q.",
    };
    struct mem_doc d = {lines, sizeof lines / sizeof lines[0]};
    paige_doc doc = { .ctx = &d,
                      .render_line = render_line,
                      .title = "memory example",
                      .raw_line = raw_line,
                      .render_line_ex = render_line_ex,
                      .line_count = line_count };
    paige_opts opts = { .chop_long_lines = 1 };
    if (paige_run(&doc, &opts) < 0) {
        for (size_t i = 0; i < d.nlines; i++) {
            (void)!write(STDOUT_FILENO, d.lines[i], strlen(d.lines[i]));
            (void)!write(STDOUT_FILENO, "\n", 1);
        }
    }
    return 0;
}
