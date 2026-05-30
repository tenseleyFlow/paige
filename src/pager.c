#include "paige.h"
#include "search.h"
#include "term.h"

#include <stdbool.h>
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
static int render_req(const paige_doc *doc, paige_stats *stats,
                      struct seglist *sl, const paige_render_req *req)
{
    sl_reset(sl);
    struct paige_sink sink = {sl};
    if (stats)
        stats->render_calls++;
    if (doc->render_line_ex)
        return doc->render_line_ex(doc->ctx, req, &sink);
    if (doc->render_line)
        return doc->render_line(doc->ctx, req->lineno, req->width, &sink);
    return 0;
}

static int render_line_matches(const paige_doc *doc, paige_stats *stats,
                               struct seglist *sl, size_t L, int w,
                               const paige_match *matches, size_t nmatches)
{
    paige_render_req req = {L, w, PAIGE_RENDER_WRAP, 0, matches, nmatches};
    return render_req(doc, stats, sl, &req);
}

static int render_line(const paige_doc *doc, paige_stats *stats,
                       struct seglist *sl, size_t L, int w)
{
    return render_line_matches(doc, stats, sl, L, w, NULL, 0);
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

static void write_counted(int fd, const char *s, size_t n, paige_stats *stats)
{
    if (stats) {
        stats->writes++;
        stats->bytes_emitted += n;
    }
    (void)!write(fd, s, n);
}

/* ---- scroll position helpers (logical line L, visual segment S) ---- */

enum { SEARCH_MAX = 256, SEARCH_STATUS_MAX = 96, SEARCH_MATCH_MAX = 64 };

enum search_dir {
    SEARCH_FORWARD = 1,
    SEARCH_BACKWARD = -1,
};

struct search_hit {
    size_t L;
    size_t off;
    size_t len;
    bool wrapped;
};

struct search_state {
    char pattern[SEARCH_MAX];
    size_t pattern_len;
    char entry[SEARCH_MAX];
    size_t entry_len;
    int dir;
    int entry_dir;
    bool entering;
    bool active;
    size_t active_L;
    size_t active_off;
    size_t active_len;
    char message[SEARCH_STATUS_MAX];
};

struct view {
    const paige_doc *doc;
    struct seglist *sl;
    int width;
    size_t L;     /* top logical line */
    int S;        /* top visual segment within L */
    long pending; /* number being typed for goto, or -1 when not entering */
    paige_stats *stats;
    struct search_state *search;
};

struct view_pos {
    size_t L;
    int S;
};

static bool draw(struct view *v, struct paige_term *t, struct outbuf *o);

static void view_save(struct view *v, struct view_pos *pos)
{
    pos->L = v->L;
    pos->S = v->S;
}

static void view_restore(struct view *v, const struct view_pos *pos)
{
    v->L = pos->L;
    v->S = pos->S;
}

static void search_clear_message(struct search_state *s)
{
    if (s)
        s->message[0] = '\0';
}

static const char *search_pattern(const struct search_state *s, size_t *len)
{
    if (s->entering) {
        *len = s->entry_len;
        return s->entry;
    }
    *len = s->pattern_len;
    return s->pattern;
}

static bool raw_line(const paige_doc *doc, size_t L, paige_line *line)
{
    if (!doc->raw_line)
        return false;
    return doc->raw_line(doc->ctx, L, line) != 0;
}

static bool search_forward_doc(struct view *v, const char *pattern,
                               size_t pattern_len, size_t start_L,
                               size_t start_off, struct search_hit *hit)
{
    bool case_sensitive = paige_search_smart_case(pattern, pattern_len);
    paige_line line;
    size_t off;

    for (size_t L = start_L; raw_line(v->doc, L, &line); L++) {
        if (v->stats)
            v->stats->search_lines++;
        size_t start = L == start_L ? start_off : 0;
        if (paige_search_find_forward(line.bytes, line.len, pattern,
                                      pattern_len, start, case_sensitive,
                                      &off)) {
            *hit = (struct search_hit){L, off, pattern_len, false};
            return true;
        }
    }
    for (size_t L = 0; raw_line(v->doc, L, &line); L++) {
        if (v->stats)
            v->stats->search_lines++;
        if (paige_search_find_forward(line.bytes, line.len, pattern,
                                      pattern_len, 0, case_sensitive, &off)) {
            *hit = (struct search_hit){L, off, pattern_len, true};
            return true;
        }
    }
    return false;
}

static bool last_raw_line(struct view *v, size_t *last)
{
    paige_line line;
    bool any = false;
    for (size_t L = 0; raw_line(v->doc, L, &line); L++) {
        any = true;
        *last = L;
        if (v->stats)
            v->stats->search_lines++;
    }
    return any;
}

static bool search_backward_doc(struct view *v, const char *pattern,
                                size_t pattern_len, size_t start_L,
                                size_t before, struct search_hit *hit)
{
    bool case_sensitive = paige_search_smart_case(pattern, pattern_len);
    paige_line line;
    size_t off;

    for (size_t i = start_L + 1; i-- > 0;) {
        if (raw_line(v->doc, i, &line)) {
            if (v->stats)
                v->stats->search_lines++;
            size_t limit = i == start_L ? before : (size_t)-1;
            if (paige_search_find_backward(line.bytes, line.len, pattern,
                                           pattern_len, limit, case_sensitive,
                                           &off)) {
                *hit = (struct search_hit){i, off, pattern_len, false};
                return true;
            }
        }
        if (i == 0)
            break;
    }

    size_t last;
    if (!last_raw_line(v, &last))
        return false;
    for (size_t i = last + 1; i-- > 0;) {
        if (raw_line(v->doc, i, &line)) {
            if (v->stats)
                v->stats->search_lines++;
            if (paige_search_find_backward(line.bytes, line.len, pattern,
                                           pattern_len, (size_t)-1,
                                           case_sensitive, &off)) {
                *hit = (struct search_hit){i, off, pattern_len, true};
                return true;
            }
        }
        if (i == 0)
            break;
    }
    return false;
}

static bool search_doc(struct view *v, int dir, size_t start_L,
                       size_t boundary, struct search_hit *hit)
{
    size_t pattern_len;
    const char *pattern = search_pattern(v->search, &pattern_len);
    if (!v->doc->raw_line || pattern_len == 0)
        return false;
    if (dir == SEARCH_FORWARD)
        return search_forward_doc(v, pattern, pattern_len, start_L, boundary,
                                  hit);
    return search_backward_doc(v, pattern, pattern_len, start_L, boundary, hit);
}

static void search_activate(struct view *v, const struct search_hit *hit)
{
    v->search->active = true;
    v->search->active_L = hit->L;
    v->search->active_off = hit->off;
    v->search->active_len = hit->len;
    v->L = hit->L;
    v->S = 0;
    if (hit->wrapped)
        snprintf(v->search->message, sizeof v->search->message,
                 "search wrapped");
    else
        search_clear_message(v->search);
}

static void search_not_found(struct view *v)
{
    v->search->active = false;
    snprintf(v->search->message, sizeof v->search->message,
             "pattern not found");
}

static void search_run_from(struct view *v, int dir, size_t start_L,
                            size_t boundary)
{
    struct search_hit hit;
    if (search_doc(v, dir, start_L, boundary, &hit))
        search_activate(v, &hit);
    else
        search_not_found(v);
}

static void search_refresh_entry(struct view *v,
                                 const struct view_pos *origin)
{
    if (v->search->entry_len == 0) {
        view_restore(v, origin);
        v->search->active = false;
        search_clear_message(v->search);
        return;
    }
    view_restore(v, origin);
    search_run_from(v, v->search->entry_dir, origin->L,
                    v->search->entry_dir == SEARCH_FORWARD ? 0 : (size_t)-1);
    if (!v->search->active)
        view_restore(v, origin);
}

static void search_copy_entry_to_pattern(struct search_state *s)
{
    memcpy(s->pattern, s->entry, s->entry_len);
    s->pattern[s->entry_len] = '\0';
    s->pattern_len = s->entry_len;
    s->dir = s->entry_dir;
}

static int search_enter(struct view *v, struct paige_term *t, struct outbuf *o,
                        int dir)
{
    if (!v->doc->raw_line) {
        snprintf(v->search->message, sizeof v->search->message,
                 "search unavailable");
        return PK_NONE;
    }

    struct view_pos origin;
    view_save(v, &origin);
    struct search_state saved = *v->search;
    v->search->entering = true;
    v->search->entry_dir = dir;
    v->search->entry_len = 0;
    v->search->entry[0] = '\0';
    v->search->active = false;
    search_clear_message(v->search);

    for (;;) {
        draw(v, t, o);
        write_counted(t->out_fd, o->p, o->len, v->stats);

        int key = paige_term_key_input(t);
        if (key == PK_CHAR) {
            if (v->search->entry_len + 1 < sizeof v->search->entry) {
                v->search->entry[v->search->entry_len++] = (char)t->ch;
                v->search->entry[v->search->entry_len] = '\0';
                search_refresh_entry(v, &origin);
            } else {
                snprintf(v->search->message, sizeof v->search->message,
                         "pattern too long");
            }
            continue;
        }
        if (key == PK_BACKSPACE) {
            if (v->search->entry_len > 0) {
                v->search->entry_len--;
                v->search->entry[v->search->entry_len] = '\0';
                search_refresh_entry(v, &origin);
            }
            continue;
        }
        if (key == PK_ENTER) {
            if (v->search->entry_len == 0) {
                *v->search = saved;
                view_restore(v, &origin);
            } else {
                bool found = v->search->active;
                char message[SEARCH_STATUS_MAX];
                memcpy(message, v->search->message, sizeof message);
                search_copy_entry_to_pattern(v->search);
                v->search->entering = false;
                if (!found)
                    memcpy(v->search->message, message, sizeof message);
            }
            return PK_NONE;
        }
        if (key == PK_ESC || key == PK_QUIT) {
            *v->search = saved;
            view_restore(v, &origin);
            return PK_NONE;
        }
        if (key == PK_RESIZE) {
            v->width = t->cols;
            continue;
        }
    }
}

static void search_repeat(struct view *v, int dir)
{
    if (!v->doc->raw_line) {
        snprintf(v->search->message, sizeof v->search->message,
                 "search unavailable");
        return;
    }
    if (v->search->pattern_len == 0) {
        snprintf(v->search->message, sizeof v->search->message,
                 "no search pattern");
        return;
    }
    search_clear_message(v->search);
    if (dir == SEARCH_FORWARD) {
        size_t start_L = v->search->active ? v->search->active_L : v->L;
        size_t start_off = v->search->active ? v->search->active_off + 1 : 0;
        search_run_from(v, SEARCH_FORWARD, start_L, start_off);
    } else {
        size_t start_L = v->search->active ? v->search->active_L : v->L;
        size_t before = v->search->active ? v->search->active_off : (size_t)-1;
        search_run_from(v, SEARCH_BACKWARD, start_L, before);
    }
}

static size_t collect_matches(struct view *v, size_t L, paige_match *matches,
                              size_t cap)
{
    if (!v->search || !v->doc->raw_line)
        return 0;
    size_t pattern_len;
    const char *pattern = search_pattern(v->search, &pattern_len);
    if (pattern_len == 0)
        return 0;
    paige_line line;
    if (!raw_line(v->doc, L, &line))
        return 0;
    bool case_sensitive = paige_search_smart_case(pattern, pattern_len);
    size_t off = 0, n = 0;
    while (n < cap && off <= line.len) {
        size_t found;
        if (!paige_search_find_forward(line.bytes, line.len, pattern,
                                       pattern_len, off, case_sensitive,
                                       &found))
            break;
        matches[n].off = found;
        matches[n].len = pattern_len;
        matches[n].active = v->search->active && L == v->search->active_L &&
                            found == v->search->active_off;
        n++;
        off = found + pattern_len;
        if (pattern_len == 0)
            break;
    }
    return n;
}

static int segcount(struct view *v, size_t L)
{
    return render_line(v->doc, v->stats, v->sl, L, v->width);
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

/* Jump so logical line N (1-based) is at the top, clamped to the last line.
 * Lazy: rendering line N-1 builds the index only that far; a number past EOF
 * walks forward to the final existing line (rare, only on overshoot). */
static void goto_line(struct view *v, unsigned long n)
{
    size_t target = n > 0 ? (size_t)(n - 1) : 0;
    if (segcount(v, target) == 0) {
        /* past EOF: settle on the last line that exists */
        size_t last = 0;
        while (segcount(v, last + 1) != 0)
            last++;
        target = last;
    }
    v->L = target;
    v->S = 0;
}

/* Render the visible screen into `o`; returns true if the bottom (EOF) shows.
 */
static bool draw(struct view *v, struct paige_term *t, struct outbuf *o)
{
    if (v->stats)
        v->stats->frames++;
    o->len = 0;
    ob_str(o, "\x1b[H"); /* cursor home */
    int body = t->rows - 1;
    size_t L = v->L;
    int S = v->S;
    bool at_eof = false;

    for (int row = 0; row < body; row++) {
        if (v->stats)
            v->stats->rows_drawn++;
        ob_str(o, "\x1b[K"); /* clear to end of line */
        paige_match matches[SEARCH_MATCH_MAX];
        size_t nmatches = 0;
        if (!at_eof)
            nmatches = collect_matches(v, L, matches, SEARCH_MATCH_MAX);
        int n = at_eof ? 0 : render_line_matches(v->doc, v->stats, v->sl, L,
                                                  v->width, matches, nmatches);
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
    if (v->pending >= 0)
        snprintf(num, sizeof num, "  :%ld ", v->pending);
    else if (v->search && v->search->entering)
        snprintf(num, sizeof num, "  %c%.*s%s%s ",
                 v->search->entry_dir == SEARCH_FORWARD ? '/' : '?',
                 (int)v->search->entry_len, v->search->entry,
                 v->search->message[0] ? "  " : "",
                 v->search->message);
    else if (v->search && v->search->message[0])
        snprintf(num, sizeof num, "  %s ", v->search->message);
    else
        snprintf(num, sizeof num, "  line %zu%s ", v->L + 1,
                 at_eof ? "  (END)" : "");
    ob_str(o, num);
    ob_str(o, "\x1b[0m");
    return at_eof;
}

/* Print the whole document plainly (used when it fits one screen). */
static void print_plain(const paige_doc *doc, paige_stats *stats,
                        struct seglist *sl, int width)
{
    for (size_t L = 0;; L++) {
        int n = render_line(doc, stats, sl, L, width);
        if (n == 0)
            break;
        for (int i = 0; i < n; i++) {
            write_counted(STDOUT_FILENO, sl->buf + sl->seg[i].off,
                          sl->seg[i].len, stats);
            write_counted(STDOUT_FILENO, "\n", 1, stats);
        }
    }
}

int paige_run(const paige_doc *doc, const paige_opts *opts)
{
    paige_stats *stats = opts ? opts->stats : NULL;
    if (stats)
        memset(stats, 0, sizeof *stats);
    if (!doc || (!doc->render_line && !doc->render_line_ex))
        return -1;

    struct paige_term t;
    if (!paige_term_open(&t))
        return -1;

    struct seglist sl = {0};

    /* Quit-if-one-screen: if everything fits, just print it (no alt screen). */
    if (opts && opts->quit_if_one_screen) {
        int visual = 0, fits = 1;
        for (size_t L = 0;; L++) {
            int n = render_line(doc, stats, &sl, L, t.cols);
            if (n == 0)
                break;
            visual += n;
            if (visual > t.rows - 0) { /* needs the full height */
                fits = 0;
                break;
            }
        }
        if (fits) {
            print_plain(doc, stats, &sl, t.cols);
            sl_free(&sl);
            close(t.tty_fd);
            return 0;
        }
    }

    paige_term_enter(&t);
    struct search_state search = {0};
    struct view v = {doc, &sl, t.cols, 0, 0, -1, stats, &search};
    struct outbuf o = {0};

    /* Pause between digits: a wait longer than this commits the running number
     * and starts a fresh one, so "1<pause>6" lands on 6 while "16" lands on
     * sixteen. ~600ms by default; the client may override (tests use a short
     * value for speed). */
    enum { GOTO_PAUSE_DEFAULT_MS = 600, GOTO_MAX = 1000000000L };
    int goto_pause_ms = GOTO_PAUSE_DEFAULT_MS;
    if (opts && opts->goto_pause_ms > 0)
        goto_pause_ms = opts->goto_pause_ms;

    for (;;) {
        draw(&v, &t, &o);
        write_counted(t.out_fd, o.p, o.len, stats);

        int key = paige_term_key(&t);
        int body = t.rows - 1;
    redispatch:
        if (key == PK_QUIT)
            break;
        if (key == PK_DIGIT) {
            /* Live incremental goto: each digit extends the number and jumps
             * immediately; a pause longer than GOTO_PAUSE_MS ends entry. The
             * terminating keystroke (if any) is re-dispatched so "16q" quits
             * and "16j" then scrolls. */
            long acc = 0;
            int k;
            do {
                acc = acc * 10 + t.digit;
                if (acc > GOTO_MAX)
                    acc = GOTO_MAX;
                goto_line(&v, (unsigned long)acc);
                v.pending = acc;
                draw(&v, &t, &o);
                write_counted(t.out_fd, o.p, o.len, stats);
                k = paige_term_key_timed(&t, goto_pause_ms);
            } while (k == PK_DIGIT);
            v.pending = -1;
            if (k == PK_TIMEOUT)
                continue; /* paused: commit and wait for the next command */
            key = k;
            goto redispatch; /* let the terminating key act */
        }
        switch (key) {
        case PK_SEARCH_FWD:
            (void)search_enter(&v, &t, &o, SEARCH_FORWARD);
            break;
        case PK_SEARCH_BACK:
            (void)search_enter(&v, &t, &o, SEARCH_BACKWARD);
            break;
        case PK_SEARCH_NEXT:
            search_repeat(&v, search.dir == SEARCH_BACKWARD ? SEARCH_BACKWARD
                                                            : SEARCH_FORWARD);
            break;
        case PK_SEARCH_PREV:
            search_repeat(&v, search.dir == SEARCH_BACKWARD ? SEARCH_FORWARD
                                                            : SEARCH_BACKWARD);
            break;
        case PK_DOWN:
            search_clear_message(&search);
            move_down(&v, 1);
            break;
        case PK_UP:
            search_clear_message(&search);
            move_up(&v, 1);
            break;
        case PK_PGDN:
            search_clear_message(&search);
            move_down(&v, body);
            break;
        case PK_PGUP:
            search_clear_message(&search);
            move_up(&v, body);
            break;
        case PK_HALFDOWN:
            search_clear_message(&search);
            move_down(&v, body / 2);
            break;
        case PK_HALFUP:
            search_clear_message(&search);
            move_up(&v, body / 2);
            break;
        case PK_TOP:
            search_clear_message(&search);
            v.L = 0;
            v.S = 0;
            break;
        case PK_BOTTOM:
            search_clear_message(&search);
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
