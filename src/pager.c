#include "paige.h"
#include "term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- a reusable list of the visual segments of one logical line ---- */

struct seglist {
    char *buf; /* all segment bytes, back to back */
    size_t buf_cap, buf_len;
    struct {
        size_t off, len;
    } *seg;
    int n, cap;
};

struct paige_sink {
    struct seglist *sl;
};

static void sl_reset(struct seglist *s)
{
    s->buf_len = 0;
    s->n = 0;
}

void paige_emit(paige_sink *sink, const char *bytes, size_t len)
{
    struct seglist *s = sink->sl;
    if (s->buf_len + len > s->buf_cap) {
        size_t cap = s->buf_cap ? s->buf_cap * 2 : 4096;
        while (cap < s->buf_len + len)
            cap *= 2;
        char *nb = realloc(s->buf, cap);
        if (!nb)
            return;
        s->buf = nb;
        s->buf_cap = cap;
    }
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 16;
        void *ns = realloc(s->seg, (size_t)cap * sizeof *s->seg);
        if (!ns)
            return;
        s->seg = ns;
        s->cap = cap;
    }
    memcpy(s->buf + s->buf_len, bytes, len);
    s->seg[s->n].off = s->buf_len;
    s->seg[s->n].len = len;
    s->n++;
    s->buf_len += len;
}

static void sl_free(struct seglist *s)
{
    free(s->buf);
    free(s->seg);
}

/* Render logical line L for content width `w`; returns segment count (0=EOF).
 */
static int render_line(const paige_doc *doc, struct seglist *sl, size_t L,
                       int w)
{
    sl_reset(sl);
    struct paige_sink sink = {sl};
    return doc->render_line(doc->ctx, L, w, &sink);
}

/* ---- an output accumulator we flush to the terminal in one write ---- */

struct outbuf {
    char *p;
    size_t cap, len;
};

static void ob_put(struct outbuf *o, const char *s, size_t n)
{
    if (o->len + n > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 8192;
        while (cap < o->len + n)
            cap *= 2;
        char *np = realloc(o->p, cap);
        if (!np)
            return;
        o->p = np;
        o->cap = cap;
    }
    memcpy(o->p + o->len, s, n);
    o->len += n;
}

static void ob_str(struct outbuf *o, const char *s)
{
    ob_put(o, s, strlen(s));
}

/* ---- scroll position helpers (logical line L, visual segment S) ---- */

struct view {
    const paige_doc *doc;
    struct seglist *sl;
    int width;
    size_t L; /* top logical line */
    int S;    /* top visual segment within L */
};

static int segcount(struct view *v, size_t L)
{
    return render_line(v->doc, v->sl, L, v->width);
}

static void move_down(struct view *v, int k)
{
    while (k-- > 0) {
        int n = segcount(v, v->L);
        if (n == 0)
            return;
        if (v->S + 1 < n) {
            v->S++;
        } else {
            if (segcount(v, v->L + 1) == 0)
                return; /* already at the last line */
            v->L++;
            v->S = 0;
        }
    }
}

static void move_up(struct view *v, int k)
{
    while (k-- > 0) {
        if (v->S > 0) {
            v->S--;
        } else if (v->L > 0) {
            int n = segcount(v, v->L - 1);
            v->L--;
            v->S = n > 0 ? n - 1 : 0;
        } else {
            return;
        }
    }
}

/* Render the visible screen into `o`; returns true if the bottom (EOF) shows.
 */
static bool draw(struct view *v, struct paige_term *t, struct outbuf *o)
{
    o->len = 0;
    ob_str(o, "\x1b[H"); /* cursor home */
    int body = t->rows - 1;
    size_t L = v->L;
    int S = v->S;
    bool at_eof = false;

    for (int row = 0; row < body; row++) {
        ob_str(o, "\x1b[K"); /* clear to end of line */
        int n = at_eof ? 0 : segcount(v, L);
        if (n == 0) {
            at_eof = true;
            ob_str(o, "~");
        } else {
            if (S < n)
                ob_put(o, v->sl->buf + v->sl->seg[S].off, v->sl->seg[S].len);
            if (++S >= n) {
                L++;
                S = 0;
            }
        }
        ob_str(o, "\r\n");
    }

    /* status line (reverse video) */
    ob_str(o, "\x1b[K\x1b[7m ");
    if (v->doc->title)
        ob_str(o, v->doc->title);
    char num[64];
    snprintf(num, sizeof num, "  line %zu%s ", v->L + 1,
             at_eof ? "  (END)" : "");
    ob_str(o, num);
    ob_str(o, "\x1b[0m");
    return at_eof;
}

/* Print the whole document plainly (used when it fits one screen). */
static void print_plain(const paige_doc *doc, struct seglist *sl, int width)
{
    for (size_t L = 0;; L++) {
        int n = render_line(doc, sl, L, width);
        if (n == 0)
            break;
        for (int i = 0; i < n; i++) {
            (void)!write(STDOUT_FILENO, sl->buf + sl->seg[i].off,
                         sl->seg[i].len);
            (void)!write(STDOUT_FILENO, "\n", 1);
        }
    }
}

int paige_run(const paige_doc *doc, const paige_opts *opts)
{
    struct paige_term t;
    if (!paige_term_open(&t))
        return -1;

    struct seglist sl = {0};

    /* Quit-if-one-screen: if everything fits, just print it (no alt screen). */
    if (opts && opts->quit_if_one_screen) {
        int visual = 0, fits = 1;
        for (size_t L = 0;; L++) {
            int n = render_line(doc, &sl, L, t.cols);
            if (n == 0)
                break;
            visual += n;
            if (visual > t.rows - 0) { /* needs the full height */
                fits = 0;
                break;
            }
        }
        if (fits) {
            print_plain(doc, &sl, t.cols);
            sl_free(&sl);
            close(t.tty_fd);
            return 0;
        }
    }

    paige_term_enter(&t);
    struct view v = {doc, &sl, t.cols, 0, 0};
    struct outbuf o = {0};

    for (;;) {
        draw(&v, &t, &o);
        (void)!write(t.out_fd, o.p, o.len);

        int key = paige_term_key(&t);
        int body = t.rows - 1;
        if (key == PK_QUIT)
            break;
        switch (key) {
        case PK_DOWN:
            move_down(&v, 1);
            break;
        case PK_UP:
            move_up(&v, 1);
            break;
        case PK_PGDN:
            move_down(&v, body);
            break;
        case PK_PGUP:
            move_up(&v, body);
            break;
        case PK_HALFDOWN:
            move_down(&v, body / 2);
            break;
        case PK_HALFUP:
            move_up(&v, body / 2);
            break;
        case PK_TOP:
            v.L = 0;
            v.S = 0;
            break;
        case PK_BOTTOM:
            /* go to end, then back up a screenful */
            while (segcount(&v, v.L + 1) != 0)
                v.L++;
            v.S = segcount(&v, v.L) - 1;
            if (v.S < 0)
                v.S = 0;
            move_up(&v, body - 1);
            break;
        case PK_RESIZE:
            v.width = t.cols;
            break;
        default:
            break;
        }
    }

    paige_term_leave(&t);
    close(t.tty_fd);
    sl_free(&sl);
    free(o.p);
    return 0;
}
