#include "paige.h"
#include "search.h"
#include "term.h"

#include <poll.h>
#include <stdarg.h>
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
    paige_stats *stats;
};

static void sl_reset(struct seglist *s)
{
    s->buf_len = 0;
    s->n = 0;
}

void paige_emit(paige_sink *sink, const char *bytes, size_t len)
{
    struct seglist *s = sink->sl;
    if (sink->stats)
        sink->stats->segments_emitted++;
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
    struct paige_sink sink = {sl, stats};
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
                               unsigned flags, size_t hscroll,
                               const paige_match *matches, size_t nmatches,
                               size_t seg_first, size_t seg_max)
{
    paige_render_req req = {L,       w,        flags,     hscroll,
                            matches, nmatches, seg_first, seg_max};
    return render_req(doc, stats, sl, &req);
}

/* Count the segments of line L without emitting any (seg_max == 0). */
static int render_line(const paige_doc *doc, paige_stats *stats,
                       struct seglist *sl, size_t L, int w)
{
    return render_line_matches(doc, stats, sl, L, w, PAIGE_RENDER_WRAP, 0, NULL,
                               0, 0, 0);
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

enum {
    SEARCH_MAX = 256,
    SEARCH_STATUS_MAX = 96,
    SEARCH_MATCH_MAX = 64,
    STATUS_MAX = 96,
    MARK_COUNT = 256,
    /* While typing an incremental search, scan at most this many lines per
     * keystroke so a miss on a huge document stays responsive instead of
     * walking the whole file (and wrapping) on every character. Enter runs the
     * authoritative unbounded search. */
    SEARCH_PREVIEW_MAX = 10000,
    /* On an unbounded search, peek for a cancelling keypress this often (must
     * be a power of two for the bit-mask check). */
    SEARCH_INTERRUPT_CHUNK = 1 << 16,
};

/* Sentinel max_scan: search the whole document (no per-keystroke bound). */
#define SEARCH_SCAN_ALL ((size_t)-1)

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
    bool interrupted;  /* last scan was cancelled by a keypress */
    bool wrapped_last; /* last activation wrapped past an edge */
    size_t total; /* total matches of the current pattern (0 = none/unknown) */
    size_t index; /* 1-based index of the active match within total */
    char message[SEARCH_STATUS_MAX];
};

struct view_pos {
    size_t L;
    int S;
    size_t hscroll;
};

struct mark {
    bool set;
    struct view_pos pos;
};

struct view {
    const paige_doc *doc;
    struct seglist *sl;
    int tty_fd; /* for cancelling a long search; <=0 disables */
    int width;
    bool chop;
    size_t hscroll;
    bool follow;
    unsigned long long follow_updates;
    size_t L;     /* top logical line */
    int S;        /* top visual segment within L */
    long pending; /* number being typed for goto, or -1 when not entering */
    paige_stats *stats;
    struct search_state *search;
    char message[STATUS_MAX];
    struct mark marks[MARK_COUNT];
    bool previous_set;
    struct view_pos previous;
};

static bool draw(struct view *v, struct paige_term *t, struct outbuf *o);

static void view_save(struct view *v, struct view_pos *pos)
{
    pos->L = v->L;
    pos->S = v->S;
    pos->hscroll = v->hscroll;
}

static void view_restore(struct view *v, const struct view_pos *pos)
{
    v->L = pos->L;
    v->S = pos->S;
    v->hscroll = pos->hscroll;
}

static bool view_pos_equal(const struct view_pos *a, const struct view *v)
{
    return a->L == v->L && a->S == v->S && a->hscroll == v->hscroll;
}

static void view_note_previous(struct view *v, const struct view_pos *origin)
{
    if (view_pos_equal(origin, v))
        return;
    v->previous = *origin;
    v->previous_set = true;
}

static int view_content_width(const struct view *v)
{
    if (!v->chop)
        return v->width;
    if (v->width > 2)
        return v->width - 2;
    return 1;
}

static bool line_has_right_overflow(struct view *v, size_t L, int content_w)
{
    paige_line line;
    if (!v->chop || !v->doc->raw_line ||
        !v->doc->raw_line(v->doc->ctx, L, &line))
        return false;
    return line.len > v->hscroll + (size_t)content_w;
}

static void search_clear_message(struct search_state *s)
{
    if (s)
        s->message[0] = '\0';
}

static void view_set_message(struct view *v, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v->message, sizeof v->message, fmt, ap);
    va_end(ap);
}

static void view_clear_message(struct view *v)
{
    v->message[0] = '\0';
    search_clear_message(v->search);
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

/* Non-blocking peek at the tty: if a key is waiting, consume it and report that
 * the user wants to cancel the in-progress scan. */
static bool search_interrupted(struct view *v)
{
    if (v->tty_fd <= 0)
        return false;
    struct pollfd pfd = {v->tty_fd, POLLIN, 0};
    if (poll(&pfd, 1, 0) <= 0)
        return false;
    unsigned char c;
    return read(v->tty_fd, &c, 1) > 0;
}

/* Per-line bookkeeping for the document scans: stop when the bounded preview
 * window is exhausted, or — on a long unbounded scan — when a keypress cancels
 * it (only checked every SEARCH_INTERRUPT_CHUNK lines, so it is ~free). */
static bool scan_should_stop(struct view *v, size_t *scanned, size_t max_scan)
{
    if (++*scanned >= max_scan)
        return true;
    if ((*scanned & (SEARCH_INTERRUPT_CHUNK - 1)) == 0 &&
        search_interrupted(v)) {
        v->search->interrupted = true;
        return true;
    }
    return false;
}

static bool search_forward_doc(struct view *v, const char *pattern,
                               size_t pattern_len, size_t start_L,
                               size_t start_off, struct search_hit *hit,
                               size_t max_scan)
{
    bool case_sensitive = paige_search_smart_case(pattern, pattern_len);
    paige_line line;
    size_t off;
    size_t scanned = 0;

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
        if (scan_should_stop(v, &scanned, max_scan))
            return false; /* preview window exhausted, or user cancelled */
    }
    if (max_scan != SEARCH_SCAN_ALL)
        return false; /* bounded preview: never wrap (only a committed search)
                       */
    for (size_t L = 0; raw_line(v->doc, L, &line); L++) {
        if (v->stats)
            v->stats->search_lines++;
        if (paige_search_find_forward(line.bytes, line.len, pattern,
                                      pattern_len, 0, case_sensitive, &off)) {
            *hit = (struct search_hit){L, off, pattern_len, true};
            return true;
        }
        if (scan_should_stop(v, &scanned, max_scan))
            return false;
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
                                size_t before, struct search_hit *hit,
                                size_t max_scan)
{
    bool case_sensitive = paige_search_smart_case(pattern, pattern_len);
    paige_line line;
    size_t off;
    size_t scanned = 0;

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
            if (scan_should_stop(v, &scanned, max_scan))
                return false; /* preview window exhausted, or user cancelled */
        }
        if (i == 0)
            break;
    }

    if (max_scan != SEARCH_SCAN_ALL)
        return false; /* bounded preview: never wrap (only a committed search)
                       */
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
            if (scan_should_stop(v, &scanned, max_scan))
                return false;
        }
        if (i == 0)
            break;
    }
    return false;
}

static bool search_doc(struct view *v, int dir, size_t start_L, size_t boundary,
                       struct search_hit *hit, size_t max_scan)
{
    size_t pattern_len;
    const char *pattern = search_pattern(v->search, &pattern_len);
    if (!v->doc->raw_line || pattern_len == 0)
        return false;
    if (dir == SEARCH_FORWARD)
        return search_forward_doc(v, pattern, pattern_len, start_L, boundary,
                                  hit, max_scan);
    return search_backward_doc(v, pattern, pattern_len, start_L, boundary, hit,
                               max_scan);
}

static void search_activate(struct view *v, const struct search_hit *hit)
{
    v->search->active = true;
    v->search->active_L = hit->L;
    v->search->active_off = hit->off;
    v->search->active_len = hit->len;
    v->search->wrapped_last = hit->wrapped;
    v->L = hit->L;
    v->S = 0;
    if (v->chop) {
        int w = view_content_width(v);
        size_t end = hit->off + hit->len;
        if (hit->off < v->hscroll)
            v->hscroll = hit->off;
        else if (end > v->hscroll + (size_t)w)
            v->hscroll = end > (size_t)w ? end - (size_t)w : 0;
    }
    if (hit->wrapped)
        snprintf(v->search->message, sizeof v->search->message,
                 "search wrapped");
    else
        search_clear_message(v->search);
}

static void search_not_found(struct view *v)
{
    v->search->active = false;
    v->search->total = 0;
    v->search->index = 0;
    snprintf(v->search->message, sizeof v->search->message,
             "pattern not found");
}

/* Count every occurrence of the active pattern in the document and the 1-based
 * index of the match at (active_L, active_off). Interruptible — a keypress
 * stops it and the partial count is returned. O(document), run once per
 * pattern. */
static size_t count_matches(struct view *v, size_t active_L, size_t active_off,
                            size_t *index_out)
{
    size_t plen;
    const char *pat = search_pattern(v->search, &plen);
    *index_out = 0;
    if (plen == 0 || !v->doc->raw_line)
        return 0;
    bool cs = paige_search_smart_case(pat, plen);
    paige_line line;
    size_t total = 0, scanned = 0;
    for (size_t L = 0; raw_line(v->doc, L, &line); L++) {
        size_t off = 0, m;
        while (paige_search_find_forward(line.bytes, line.len, pat, plen, off,
                                         cs, &m)) {
            total++;
            if (L < active_L || (L == active_L && m <= active_off))
                *index_out = total;
            off = m + plen; /* non-overlapping */
        }
        if (scan_should_stop(v, &scanned, SEARCH_SCAN_ALL))
            break; /* cancelled: report the partial count */
    }
    return total;
}

/* Recompute total + active index for the committed pattern (one full scan). */
static void search_update_count(struct view *v)
{
    v->search->total = count_matches(v, v->search->active_L,
                                     v->search->active_off, &v->search->index);
}

/* Advance the 1-based match index after an n/N step, using the wrap flag the
 * scan recorded (O(1), no rescan). */
static void search_step_index(struct view *v, int dir)
{
    if (v->search->total == 0)
        return;
    if (v->search->wrapped_last)
        v->search->index = (dir == SEARCH_FORWARD) ? 1 : v->search->total;
    else if (dir == SEARCH_FORWARD)
        v->search->index =
            v->search->index < v->search->total ? v->search->index + 1 : 1;
    else
        v->search->index =
            v->search->index > 1 ? v->search->index - 1 : v->search->total;
}

static bool search_run_from(struct view *v, int dir, size_t start_L,
                            size_t boundary, size_t max_scan)
{
    struct search_hit hit;
    v->search->interrupted = false;
    if (search_doc(v, dir, start_L, boundary, &hit, max_scan)) {
        search_activate(v, &hit);
        return true;
    }
    if (v->search->interrupted) {
        v->search->active = false;
        snprintf(v->search->message, sizeof v->search->message,
                 "search interrupted");
        return false;
    }
    search_not_found(v);
    return false;
}

/* Live preview while the user is still typing the pattern: bounded forward (or
 * backward) scan from the origin, no wrap-around, so each keystroke stays cheap
 * on a huge document. A miss here is silent — it may just be past the preview
 * window; Enter runs the full search and reports a real not-found. */
static void search_refresh_entry(struct view *v, const struct view_pos *origin)
{
    if (v->search->entry_len == 0) {
        view_restore(v, origin);
        v->search->active = false;
        search_clear_message(v->search);
        return;
    }
    view_restore(v, origin);
    search_run_from(v, v->search->entry_dir, origin->L,
                    v->search->entry_dir == SEARCH_FORWARD ? 0 : (size_t)-1,
                    SEARCH_PREVIEW_MAX);
    if (!v->search->active) {
        view_restore(v, origin);
        search_clear_message(v->search);
    }
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
                search_copy_entry_to_pattern(v->search);
                v->search->entering = false;
                /* Authoritative unbounded search from the origin: the live
                 * preview was bounded and may have missed a far match. */
                view_restore(v, &origin);
                bool found = search_run_from(
                    v, v->search->dir, origin.L,
                    v->search->dir == SEARCH_FORWARD ? 0 : (size_t)-1,
                    SEARCH_SCAN_ALL);
                if (found) {
                    view_note_previous(v, &origin);
                    search_update_count(v);
                }
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
    struct view_pos origin;
    view_save(v, &origin);
    bool found;
    if (dir == SEARCH_FORWARD) {
        size_t start_L = v->search->active ? v->search->active_L : v->L;
        size_t start_off = v->search->active ? v->search->active_off + 1 : 0;
        found = search_run_from(v, SEARCH_FORWARD, start_L, start_off,
                                SEARCH_SCAN_ALL);
    } else {
        size_t start_L = v->search->active ? v->search->active_L : v->L;
        size_t before = v->search->active ? v->search->active_off : (size_t)-1;
        found = search_run_from(v, SEARCH_BACKWARD, start_L, before,
                                SEARCH_SCAN_ALL);
    }
    if (found) {
        view_note_previous(v, &origin);
        search_step_index(v, dir);
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
    if (!v->chop)
        return render_line(v->doc, v->stats, v->sl, L, v->width);
    return render_line_matches(v->doc, v->stats, v->sl, L,
                               view_content_width(v), PAIGE_RENDER_CHOP,
                               v->hscroll, NULL, 0, 0, 0);
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
static void goto_line(struct view *v, size_t n)
{
    size_t target = n > 0 ? n - 1 : 0;
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

static void view_clamp(struct view *v)
{
    int n = segcount(v, v->L);
    if (n == 0) {
        v->L = 0;
        v->S = 0;
        return;
    }
    if (v->S < 0)
        v->S = 0;
    if (v->S >= n)
        v->S = n - 1;
}

static size_t percent_target(size_t count, unsigned long pct)
{
    if (pct > 100)
        pct = 100;
    size_t last = count - 1;
    return (last / 100) * pct + ((last % 100) * pct) / 100;
}

static bool goto_percent(struct view *v, unsigned long pct)
{
    size_t count = 0;
    if (!v->doc->line_count || !v->doc->line_count(v->doc->ctx, &count) ||
        count == 0) {
        view_set_message(v, "percent unavailable");
        return false;
    }
    goto_line(v, percent_target(count, pct) + 1);
    return true;
}

static void goto_bottom(struct view *v, int body)
{
    size_t count = 0;
    if (v->doc->line_count && v->doc->line_count(v->doc->ctx, &count) &&
        count > 0) {
        /* Known length: jump straight to the last line and fill the screen
         * upward. segcount renders only that one line plus the screenful
         * move_up walks — O(screen), not a full-document scan. */
        v->L = count - 1;
        int n = segcount(v, v->L);
        v->S = n > 0 ? n - 1 : 0;
        move_up(v, body - 1);
        return;
    }
    /* Unknown length (a streaming host with no line_count): we have to scan to
     * find EOF, the same as `less +G` on a pipe. */
    if (segcount(v, v->L) == 0) {
        v->L = 0;
        v->S = 0;
    }
    while (segcount(v, v->L + 1) != 0)
        v->L++;
    v->S = segcount(v, v->L) - 1;
    if (v->S < 0)
        v->S = 0;
    move_up(v, body - 1);
}

static bool follow_refresh(struct view *v, int body)
{
    if (!v->follow || !v->doc->refresh)
        return false;
    if (v->stats)
        v->stats->follow_refreshes++;
    if (!v->doc->refresh(v->doc->ctx))
        return false;
    if (v->stats)
        v->stats->follow_updates++;
    v->follow_updates++;
    goto_bottom(v, body);
    return true;
}

static void follow_start(struct view *v, int body)
{
    if (!v->doc->refresh) {
        view_set_message(v, "follow unavailable");
        return;
    }
    view_clear_message(v);
    v->follow = true;
    (void)follow_refresh(v, body);
    goto_bottom(v, body);
}

static void follow_pause(struct view *v)
{
    v->follow = false;
}

static void move_left(struct view *v, size_t cols)
{
    if (!v->chop)
        return;
    if (v->stats)
        v->stats->hscroll_moves++;
    if (v->hscroll > cols)
        v->hscroll -= cols;
    else
        v->hscroll = 0;
    v->S = 0;
}

static void move_right(struct view *v, size_t cols)
{
    if (!v->chop)
        return;
    if (v->stats)
        v->stats->hscroll_moves++;
    v->hscroll += cols;
    v->S = 0;
}

static void jump_to_pos(struct view *v, const struct view_pos *pos)
{
    struct view_pos origin;
    view_save(v, &origin);
    view_restore(v, pos);
    view_clamp(v);
    view_note_previous(v, &origin);
}

static void mark_set(struct view *v, unsigned char mark)
{
    view_save(v, &v->marks[mark].pos);
    v->marks[mark].set = true;
    view_set_message(v, "mark %c set", mark);
}

static void mark_jump(struct view *v, unsigned char mark, int body)
{
    if (mark == '\'') {
        if (v->previous_set)
            jump_to_pos(v, &v->previous);
        else
            view_set_message(v, "no previous position");
        return;
    }
    if (mark == '^') {
        struct view_pos origin;
        view_save(v, &origin);
        v->L = 0;
        v->S = 0;
        view_note_previous(v, &origin);
        return;
    }
    if (mark == '$') {
        struct view_pos origin;
        view_save(v, &origin);
        goto_bottom(v, body);
        view_note_previous(v, &origin);
        return;
    }
    if (mark == '.') {
        view_set_message(v, "line %zu", v->L + 1);
        return;
    }
    if (!v->marks[mark].set) {
        view_set_message(v, "mark %c not set", mark);
        return;
    }
    jump_to_pos(v, &v->marks[mark].pos);
}

static int mark_enter(struct view *v, struct paige_term *t, struct outbuf *o,
                      bool set, int body)
{
    for (;;) {
        view_set_message(v, set ? "set mark" : "jump to mark");
        draw(v, t, o);
        write_counted(t->out_fd, o->p, o->len, v->stats);

        int key = paige_term_key_input(t);
        if (key == PK_RESIZE) {
            v->width = t->cols;
            continue;
        }
        if (key == PK_QUIT)
            return PK_QUIT;
        if (key == PK_ESC) {
            v->message[0] = '\0';
            return PK_NONE;
        }
        if (key != PK_CHAR) {
            view_set_message(v, "mark cancelled");
            return PK_NONE;
        }
        if (set)
            mark_set(v, t->ch);
        else
            mark_jump(v, t->ch, body);
        return PK_NONE;
    }
}

struct static_doc {
    const char *const *lines;
    size_t nlines;
};

static int static_render_line(void *ctx, size_t L, int width, paige_sink *sink)
{
    struct static_doc *d = ctx;
    if (L >= d->nlines)
        return 0;
    if (width < 1)
        width = 1;
    const char *line = d->lines[L];
    size_t len = strlen(line);
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

static void static_doc_enter(struct view *v, struct paige_term *t,
                             struct outbuf *o, const char *title,
                             const char *const *lines, size_t nlines)
{
    struct static_doc sd = {lines, nlines};
    paige_doc static_doc = {
        .ctx = &sd, .render_line = static_render_line, .title = title};
    struct search_state static_search = {0};
    struct view sv = {.doc = &static_doc,
                      .sl = v->sl,
                      .width = t->cols,
                      .chop = false,
                      .hscroll = 0,
                      .L = 0,
                      .S = 0,
                      .pending = -1,
                      .stats = v->stats,
                      .search = &static_search};

    for (;;) {
        draw(&sv, t, o);
        write_counted(t->out_fd, o->p, o->len, v->stats);

        int key = paige_term_key(t);
        int body = t->rows - 1;
        switch (key) {
        case PK_QUIT:
        case PK_ESC:
        case PK_HELP:
        case PK_PERF:
            return;
        case PK_DOWN:
            view_clear_message(&sv);
            move_down(&sv, 1);
            break;
        case PK_UP:
            view_clear_message(&sv);
            move_up(&sv, 1);
            break;
        case PK_PGDN:
            view_clear_message(&sv);
            move_down(&sv, body);
            break;
        case PK_PGUP:
            view_clear_message(&sv);
            move_up(&sv, body);
            break;
        case PK_HALFDOWN:
            view_clear_message(&sv);
            move_down(&sv, body / 2);
            break;
        case PK_HALFUP:
            view_clear_message(&sv);
            move_up(&sv, body / 2);
            break;
        case PK_TOP:
            view_clear_message(&sv);
            sv.L = 0;
            sv.S = 0;
            break;
        case PK_BOTTOM:
            view_clear_message(&sv);
            goto_bottom(&sv, body);
            break;
        case PK_RESIZE:
            sv.width = t->cols;
            break;
        default:
            view_set_message(&sv, "q or Esc returns");
            break;
        }
    }
}

static void help_enter(struct view *v, struct paige_term *t, struct outbuf *o)
{
    static const char *const help_lines[] = {
        "paige help",
        "",
        "Navigation:",
        "  j/k or arrows      scroll one row",
        "  space/f, b         page down/up",
        "  d/u                half-page down/up",
        "  g/G                top/bottom",
        "  digits             jump to line as you type",
        "  digits%            jump to a known percentage",
        "",
        "Search:",
        "  /, ?               forward/backward search",
        "  n, N               repeat search",
        "",
        "Marks:",
        "  m<char>            set mark",
        "  '<char>            jump to mark",
        "  ''                 previous position",
        "  '^, '$, '.         top, bottom, current line",
        "",
        "Performance:",
        "  P                  open performance panel",
        "",
        "Chop mode:",
        "  left/right arrows  horizontal scroll",
        "",
        "Help:",
        "  h                  open this help",
        "  q or Esc           return",
    };
    static_doc_enter(v, t, o, "paige help", help_lines,
                     sizeof help_lines / sizeof help_lines[0]);
}

static void perf_enter(struct view *v, struct paige_term *t, struct outbuf *o)
{
    char pos[96], render[96], output[96], search[96], follow[96], hint[96];
    const char *lines[16];
    size_t n = 0;
    lines[n++] = "paige performance";
    lines[n++] = "";
    snprintf(pos, sizeof pos, "view: line=%zu segment=%d hscroll=%zu follow=%s",
             v->L + 1, v->S, v->hscroll + 1, v->follow ? "yes" : "no");
    lines[n++] = pos;
    if (v->stats) {
        snprintf(
            render, sizeof render, "render: calls=%llu frames=%llu rows=%llu",
            v->stats->render_calls, v->stats->frames, v->stats->rows_drawn);
        snprintf(output, sizeof output, "terminal: writes=%llu bytes=%llu",
                 v->stats->writes, v->stats->bytes_emitted);
        snprintf(search, sizeof search,
                 "search: scanned_lines=%llu hscroll_moves=%llu",
                 v->stats->search_lines, v->stats->hscroll_moves);
        snprintf(follow, sizeof follow, "follow: refreshes=%llu updates=%llu",
                 v->stats->follow_refreshes, v->stats->follow_updates);
        lines[n++] = render;
        lines[n++] = output;
        lines[n++] = search;
        lines[n++] = follow;
    } else {
        lines[n++] =
            "stats unavailable: pass paige_opts.stats to collect counters";
    }
    lines[n++] = "";
    snprintf(hint, sizeof hint, "bench: run sh bench/pager.sh --help");
    lines[n++] = hint;
    lines[n++] = "q or Esc returns";

    static_doc_enter(v, t, o, "paige performance", lines, n);
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

    int content_w = view_content_width(v);
    unsigned flags = v->chop ? PAIGE_RENDER_CHOP : PAIGE_RENDER_WRAP;
    paige_match matches[SEARCH_MATCH_MAX];
    size_t nmatches = 0;
    int row = 0;

    /* Render each line once, asking only for the visible window of its segments
     * [S .. body). A renderer that honors the window keeps a long wrapped line
     * O(visible); one that emits every segment still works — we detect the full
     * emission (sl->n == total) and index from S instead of from 0. */
    while (row < body && !at_eof) {
        nmatches = collect_matches(v, L, matches, SEARCH_MATCH_MAX);
        int total = render_line_matches(v->doc, v->stats, v->sl, L, content_w,
                                        flags, v->hscroll, matches, nmatches,
                                        (size_t)S, (size_t)(body - row));
        if (total == 0) {
            at_eof = true;
            break;
        }
        bool windowed = v->sl->n < total;
        int base = windowed ? 0 : S; /* seglist index of absolute segment S */
        int avail = v->sl->n - base;
        bool overflow = v->chop && line_has_right_overflow(v, L, content_w);
        int shown = 0;
        while (shown < avail && row < body) {
            if (v->stats)
                v->stats->rows_drawn++;
            ob_str(o, "\x1b[K"); /* clear to end of line */
            if (v->chop)
                ob_str(o, v->hscroll > 0 ? "<" : " ");
            ob_put(o, v->sl->buf + v->sl->seg[base + shown].off,
                   v->sl->seg[base + shown].len);
            if (overflow)
                ob_str(o, ">");
            ob_str(o, "\r\n");
            shown++;
            row++;
        }
        if (avail <= 0 || S + shown >= total) {
            L++;
            S = 0;
        } else {
            S += shown;
        }
    }
    for (; row < body; row++) {
        if (v->stats)
            v->stats->rows_drawn++;
        ob_str(o, "\x1b[K~\r\n");
        at_eof = true;
    }

    /* status line (reverse video) */
    ob_str(o, "\x1b[K\x1b[7m ");
    if (v->doc->title)
        ob_str(o, v->doc->title);
    char num[160];
    if (v->pending >= 0)
        snprintf(num, sizeof num, "  :%ld ", v->pending);
    else if (v->search && v->search->entering)
        snprintf(num, sizeof num, "  %c%.*s%s%s ",
                 v->search->entry_dir == SEARCH_FORWARD ? '/' : '?',
                 (int)v->search->entry_len, v->search->entry,
                 v->search->message[0] ? "  " : "", v->search->message);
    else if (v->message[0])
        snprintf(num, sizeof num, "  %s ", v->message);
    else if (v->search && v->search->message[0])
        snprintf(num, sizeof num, "  %s ", v->search->message);
    else if (v->follow && v->chop && v->follow_updates == 0)
        snprintf(num, sizeof num, "  line %zu  col %zu  (FOLLOW waiting)%s ",
                 v->L + 1, v->hscroll + 1, at_eof ? "  (END)" : "");
    else if (v->follow && v->chop)
        snprintf(num, sizeof num,
                 "  line %zu  col %zu  (FOLLOW updates %llu)%s ", v->L + 1,
                 v->hscroll + 1, v->follow_updates, at_eof ? "  (END)" : "");
    else if (v->follow && v->follow_updates == 0)
        snprintf(num, sizeof num, "  line %zu  (FOLLOW waiting)%s ", v->L + 1,
                 at_eof ? "  (END)" : "");
    else if (v->follow)
        snprintf(num, sizeof num, "  line %zu  (FOLLOW updates %llu)%s ",
                 v->L + 1, v->follow_updates, at_eof ? "  (END)" : "");
    else if (v->chop)
        snprintf(num, sizeof num, "  line %zu  col %zu%s ", v->L + 1,
                 v->hscroll + 1, at_eof ? "  (END)" : "");
    else
        snprintf(num, sizeof num, "  line %zu%s ", v->L + 1,
                 at_eof ? "  (END)" : "");
    ob_str(o, num);
    /* search overview: which match of how many. */
    if (v->search && v->search->active && v->search->total > 0 &&
        !v->search->entering && v->search->message[0] == '\0') {
        char minfo[48];
        snprintf(minfo, sizeof minfo, " [%zu/%zu] ", v->search->index,
                 v->search->total);
        ob_str(o, minfo);
    }
    ob_str(o, "\x1b[0m");
    return at_eof;
}

/* Print the whole document plainly (used when it fits one screen). */
static void print_plain(const paige_doc *doc, paige_stats *stats,
                        struct seglist *sl, int width)
{
    for (size_t L = 0;; L++) {
        /* Emit every segment (seg_max = all), not count-only — print_plain
         * actually writes the seglist, unlike segcount. */
        int n = render_line_matches(doc, stats, sl, L, width, PAIGE_RENDER_WRAP,
                                    0, NULL, 0, 0, (size_t)-1);
        if (n == 0)
            break;
        for (int i = 0; i < sl->n; i++) {
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
    struct view v = {.doc = doc,
                     .sl = &sl,
                     .tty_fd = t.tty_fd,
                     .width = t.cols,
                     .chop =
                         opts && opts->chop_long_lines && doc->render_line_ex,
                     .hscroll = 0,
                     .L = 0,
                     .S = 0,
                     .pending = -1,
                     .stats = stats,
                     .search = &search};
    struct outbuf o = {0};

    /* Pause between digits: a wait longer than this commits the running number
     * and starts a fresh one, so "1<pause>6" lands on 6 while "16" lands on
     * sixteen. ~600ms by default; the client may override (tests use a short
     * value for speed). */
    enum {
        GOTO_PAUSE_DEFAULT_MS = 600,
        FOLLOW_POLL_DEFAULT_MS = 250,
        GOTO_MAX = 1000000000L,
    };
    int goto_pause_ms = GOTO_PAUSE_DEFAULT_MS;
    if (opts && opts->goto_pause_ms > 0)
        goto_pause_ms = opts->goto_pause_ms;
    int follow_poll_ms = FOLLOW_POLL_DEFAULT_MS;
    if (opts && opts->follow_poll_ms > 0)
        follow_poll_ms = opts->follow_poll_ms;

    bool dirty = true;
    for (;;) {
        if (dirty) {
            draw(&v, &t, &o);
            write_counted(t.out_fd, o.p, o.len, stats);
            dirty = false;
        }

        int key = v.follow ? paige_term_key_timed(&t, follow_poll_ms)
                           : paige_term_key(&t);
        int body = t.rows - 1;
        if (key == PK_TIMEOUT) {
            if (follow_refresh(&v, body))
                dirty = true;
            continue;
        }
    redispatch:
        if (key == PK_QUIT)
            break;
        if (key == PK_DIGIT) {
            /* Live incremental goto: each digit extends the number and jumps
             * immediately; a pause longer than GOTO_PAUSE_MS ends entry. The
             * terminating keystroke (if any) is re-dispatched so "16q" quits
             * and "16j" then scrolls. */
            follow_pause(&v);
            view_clear_message(&v);
            struct view_pos origin;
            view_save(&v, &origin);
            long acc = 0;
            int k;
            do {
                acc = acc * 10 + t.digit;
                if (acc > GOTO_MAX)
                    acc = GOTO_MAX;
                goto_line(&v, (size_t)acc);
                v.pending = acc;
                draw(&v, &t, &o);
                write_counted(t.out_fd, o.p, o.len, stats);
                k = paige_term_key_timed(&t, goto_pause_ms);
            } while (k == PK_DIGIT);
            v.pending = -1;
            if (k == PK_PERCENT) {
                view_restore(&v, &origin);
                if (goto_percent(&v, (unsigned long)acc))
                    view_note_previous(&v, &origin);
                dirty = true;
                continue;
            }
            view_note_previous(&v, &origin);
            if (k == PK_TIMEOUT) {
                dirty = true;
                continue; /* paused: commit and wait for the next command */
            }
            key = k;
            goto redispatch; /* let the terminating key act */
        }
        switch (key) {
        case PK_RIGHT:
            follow_pause(&v);
            view_clear_message(&v);
            move_right(&v, 8);
            break;
        case PK_LEFT:
            follow_pause(&v);
            view_clear_message(&v);
            move_left(&v, 8);
            break;
        case PK_FOLLOW:
            follow_start(&v, body);
            break;
        case PK_PERCENT:
            follow_pause(&v);
            view_set_message(&v, "type digits then %%");
            break;
        case PK_MARK_SET:
            follow_pause(&v);
            if (mark_enter(&v, &t, &o, true, body) == PK_QUIT)
                goto done;
            break;
        case PK_MARK_JUMP:
            follow_pause(&v);
            if (mark_enter(&v, &t, &o, false, body) == PK_QUIT)
                goto done;
            break;
        case PK_HELP:
            follow_pause(&v);
            view_clear_message(&v);
            help_enter(&v, &t, &o);
            break;
        case PK_PERF:
            follow_pause(&v);
            view_clear_message(&v);
            perf_enter(&v, &t, &o);
            break;
        case PK_SEARCH_FWD:
            follow_pause(&v);
            view_clear_message(&v);
            (void)search_enter(&v, &t, &o, SEARCH_FORWARD);
            break;
        case PK_SEARCH_BACK:
            follow_pause(&v);
            view_clear_message(&v);
            (void)search_enter(&v, &t, &o, SEARCH_BACKWARD);
            break;
        case PK_SEARCH_NEXT:
            follow_pause(&v);
            v.message[0] = '\0';
            search_repeat(&v, search.dir == SEARCH_BACKWARD ? SEARCH_BACKWARD
                                                            : SEARCH_FORWARD);
            break;
        case PK_SEARCH_PREV:
            follow_pause(&v);
            v.message[0] = '\0';
            search_repeat(&v, search.dir == SEARCH_BACKWARD ? SEARCH_FORWARD
                                                            : SEARCH_BACKWARD);
            break;
        case PK_DOWN:
            follow_pause(&v);
            view_clear_message(&v);
            move_down(&v, 1);
            break;
        case PK_UP:
            follow_pause(&v);
            view_clear_message(&v);
            move_up(&v, 1);
            break;
        case PK_PGDN:
            follow_pause(&v);
            view_clear_message(&v);
            move_down(&v, body);
            break;
        case PK_PGUP:
            follow_pause(&v);
            view_clear_message(&v);
            move_up(&v, body);
            break;
        case PK_HALFDOWN:
            follow_pause(&v);
            view_clear_message(&v);
            move_down(&v, body / 2);
            break;
        case PK_HALFUP:
            follow_pause(&v);
            view_clear_message(&v);
            move_up(&v, body / 2);
            break;
        case PK_TOP:
            follow_pause(&v);
            view_clear_message(&v);
            {
                struct view_pos origin;
                view_save(&v, &origin);
                v.L = 0;
                v.S = 0;
                view_note_previous(&v, &origin);
            }
            break;
        case PK_BOTTOM:
            follow_pause(&v);
            view_clear_message(&v);
            {
                struct view_pos origin;
                view_save(&v, &origin);
                goto_bottom(&v, body);
                view_note_previous(&v, &origin);
            }
            break;
        case PK_RESIZE:
            v.width = t.cols;
            view_clamp(&v);
            break;
        case PK_OTHER:
            view_set_message(&v, "unknown command");
            break;
        default:
            break;
        }
        dirty = true;
    }

done:
    paige_term_leave(&t);
    close(t.tty_fd);
    sl_free(&sl);
    free(o.p);
    return 0;
}
