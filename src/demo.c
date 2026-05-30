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

static int render_line(void *ctx, size_t L, int width, paige_sink *sink)
{
    struct doc *d = ctx;
    if (L >= d->nlines)
        return 0;
    size_t start = d->line[L];
    size_t end = (L + 1 < d->nlines) ? d->line[L + 1] - 1 : d->size;
    size_t len = end > start ? end - start : 0;
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
    struct doc d;
    memset(&d, 0, sizeof d);
    d.data = slurp(path, &d.size);
    if (d.data == NULL) {
        fprintf(stderr, "paige-demo: cannot read %s\n", path ? path : "stdin");
        return 1;
    }
    d.title = path ? path : "stdin";
    index_lines(&d);

    paige_doc doc = {&d, render_line, d.title};
    paige_opts opts = {1};
    if (paige_run(&doc, &opts) < 0) {
        /* No terminal: dump plainly. */
        (void)!write(STDOUT_FILENO, d.data, d.size);
    }
    free(d.data);
    free(d.line);
    return 0;
}
