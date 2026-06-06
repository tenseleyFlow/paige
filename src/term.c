#include "term.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>

/* SIGWINCH sets this; the key loop turns it into PK_RESIZE. */
static volatile sig_atomic_t paige_resized = 0;

static void on_winch(int sig)
{
    (void)sig;
    paige_resized = 1;
}

/* The active terminal, for the fatal-signal restore path. */
static struct paige_term *g_active = NULL;

/*
 * Put the terminal back if a fatal signal kills us mid-page. Without this an
 * external SIGTERM/SIGHUP (or any signal whose default action terminates the
 * process) leaves the user on the alternate screen with the cursor hidden and
 * raw mode set. Installed with SA_RESETHAND, so once we have restored we
 * re-raise to die with the signal's normal disposition and exit status.
 */
static void on_fatal(int sig)
{
    if (g_active) {
        paige_term_leave(g_active);
        g_active = NULL;
    }
    raise(sig);
}

static void write_all(int fd, const char *s)
{
    size_t n = strlen(s);
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        s += w;
        n -= (size_t)w;
    }
}

bool paige_term_open(struct paige_term *t)
{
    memset(t, 0, sizeof *t);
    t->out_fd = STDOUT_FILENO;
    t->tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (t->tty_fd < 0)
        return false;
    if (!isatty(t->out_fd)) {
        close(t->tty_fd);
        t->tty_fd = -1;
        return false;
    }
    paige_term_size(t);
    return true;
}

void paige_term_size(struct paige_term *t)
{
    struct winsize ws;
    if (ioctl(t->out_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 &&
        ws.ws_col > 0) {
        t->rows = ws.ws_row;
        t->cols = ws.ws_col;
    } else {
        t->rows = 24;
        t->cols = 80;
    }
}

void paige_term_enter(struct paige_term *t)
{
    if (tcgetattr(t->tty_fd, &t->orig) == 0) {
        struct termios raw = t->orig;
        /* Clear ISIG so Ctrl-C/Ctrl-\ reach us as bytes (the pager quits on
         * them via the key loop and restores the terminal) instead of killing
         * the process with the terminal still in raw/alt mode; clear IXON so
         * Ctrl-S can't silently freeze pager output. */
        raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO | ISIG);
        raw.c_iflag &= (tcflag_t)~IXON;
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(t->tty_fd, TCSAFLUSH, &raw) == 0)
            t->raw = true;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler =
        on_winch; /* no SA_RESTART: read() returns EINTR on resize */
    sigaction(SIGWINCH, &sa, NULL);

    /* Restore the terminal if we are killed by a signal whose default action
     * terminates us (an external SIGTERM/SIGHUP, or SIGINT/SIGQUIT delivered
     * from outside the tty). */
    struct sigaction fa;
    memset(&fa, 0, sizeof fa);
    fa.sa_handler = on_fatal;
    fa.sa_flags = SA_RESETHAND; /* fire once, then default disposition */
    sigaction(SIGINT, &fa, NULL);
    sigaction(SIGTERM, &fa, NULL);
    sigaction(SIGQUIT, &fa, NULL);
    sigaction(SIGHUP, &fa, NULL);
    g_active = t;

    write_all(t->out_fd, "\x1b[?1049h" /* alt screen */
                         "\x1b[?25l" /* hide cursor */);
    t->alt = true;
}

void paige_term_leave(struct paige_term *t)
{
    if (t->alt) {
        write_all(t->out_fd, "\x1b[?25h" /* show cursor */
                             "\x1b[?1049l" /* leave alt screen */);
        t->alt = false;
    }
    if (t->raw) {
        tcsetattr(t->tty_fd, TCSAFLUSH, &t->orig);
        t->raw = false;
    }
    g_active = NULL;
}

/* Read one byte from the tty, or -1 on EINTR/EOF. */
static int read_byte(struct paige_term *t)
{
    unsigned char c;
    ssize_t r = read(t->tty_fd, &c, 1);
    if (r == 1)
        return c;
    return -1;
}

enum { READ_TIMEOUT = -2, ESC_TIMEOUT_MS = 25 };

static int read_byte_deadline(struct paige_term *t, int timeout_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(t->tty_fd, &rfds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int sr = select(t->tty_fd + 1, &rfds, NULL, NULL, &tv);
    if (sr == 0)
        return READ_TIMEOUT;
    if (sr < 0)
        return -1;
    return read_byte(t);
}

/* Decode an ESC [ ... sequence into an action. */
static int decode_escape(struct paige_term *t, bool input_mode)
{
    int b = read_byte_deadline(t, ESC_TIMEOUT_MS);
    if (b == READ_TIMEOUT)
        return PK_ESC;
    if (b != '[' && b != 'O')
        return PK_ESC;
    int c = read_byte_deadline(t, ESC_TIMEOUT_MS);
    if (c == READ_TIMEOUT)
        return PK_ESC;
    switch (c) {
    case 'A':
        return PK_UP;
    case 'B':
        return PK_DOWN;
    case 'C':
        return PK_RIGHT;
    case 'D':
        return PK_LEFT;
    case 'H':
        return input_mode ? PK_HOME : PK_TOP;
    case 'F':
        return input_mode ? PK_END : PK_BOTTOM;
    case '1':
        read_byte_deadline(t, ESC_TIMEOUT_MS); /* consume '~' */
        return input_mode ? PK_HOME : PK_TOP;
    case '3':
        read_byte_deadline(t, ESC_TIMEOUT_MS);
        return PK_DELETE;
    case '4':
        read_byte_deadline(t, ESC_TIMEOUT_MS);
        return input_mode ? PK_END : PK_BOTTOM;
    case '5':
        read_byte_deadline(t, ESC_TIMEOUT_MS); /* consume '~' */
        return PK_PGUP;
    case '6':
        read_byte_deadline(t, ESC_TIMEOUT_MS);
        return PK_PGDN;
    default:
        return PK_OTHER;
    }
}

/* Map one input byte to an action. */
int paige_term_decode_command_byte(struct paige_term *t, unsigned char c)
{
    if (c >= '0' && c <= '9') {
        t->digit = c - '0';
        return PK_DIGIT;
    }
    switch (c) {
    case 'q':
    case 'Q':
    case 3: /* Ctrl-C */
        return PK_QUIT;
    case 'j':
    case '\n':
    case '\r':
        return PK_DOWN;
    case 'k':
        return PK_UP;
    case '/':
        return PK_SEARCH_FWD;
    case '?':
        return PK_SEARCH_BACK;
    case 'n':
        return PK_SEARCH_NEXT;
    case 'N':
        return PK_SEARCH_PREV;
    case '%':
        return PK_PERCENT;
    case 'm':
        return PK_MARK_SET;
    case '\'':
        return PK_MARK_JUMP;
    case 'h':
        return PK_HELP;
    case 'F':
        return PK_FOLLOW;
    case 'P':
        return PK_PERF;
    case '|':
        return PK_RULER;
    case ']':
        return PK_LANDMARK_NEXT;
    case '[':
        return PK_LANDMARK_PREV;
    case ' ':
    case 'f':
        return PK_PGDN;
    case 'b':
        return PK_PGUP;
    case 'd':
        return PK_HALFDOWN;
    case 'u':
        return PK_HALFUP;
    case 'g':
        return PK_TOP;
    case 'G':
        return PK_BOTTOM;
    case 0x1b:
        return decode_escape(t, false);
    default:
        return PK_OTHER;
    }
}

int paige_term_decode_input_byte(struct paige_term *t, unsigned char c)
{
    switch (c) {
    case 3: /* Ctrl-C */
        return PK_QUIT;
    case '\n':
    case '\r':
        return PK_ENTER;
    case 0x08:
    case 0x7f:
        return PK_BACKSPACE;
    case 0x1b:
        return decode_escape(t, true);
    default:
        if (c >= 0x20 && c <= 0x7e) {
            t->ch = c;
            return PK_CHAR;
        }
        return PK_OTHER;
    }
}

int paige_term_key(struct paige_term *t)
{
    for (;;) {
        unsigned char c;
        ssize_t r = read(t->tty_fd, &c, 1);
        if (r < 0) {
            if (errno == EINTR) {
                if (paige_resized) {
                    paige_resized = 0;
                    paige_term_size(t);
                    return PK_RESIZE;
                }
                continue;
            }
            return PK_QUIT;
        }
        if (r == 0)
            return PK_QUIT;
        return paige_term_decode_command_byte(t, c);
    }
}

int paige_term_key_input(struct paige_term *t)
{
    for (;;) {
        unsigned char c;
        ssize_t r = read(t->tty_fd, &c, 1);
        if (r < 0) {
            if (errno == EINTR) {
                if (paige_resized) {
                    paige_resized = 0;
                    paige_term_size(t);
                    return PK_RESIZE;
                }
                continue;
            }
            return PK_QUIT;
        }
        if (r == 0)
            return PK_QUIT;
        return paige_term_decode_input_byte(t, c);
    }
}

int paige_term_key_timed(struct paige_term *t, int timeout_ms)
{
    /* select(), not poll(): poll() on a tty does not reliably honor the timeout
     * on macOS, which left the digit-goto entry waiting forever for the next
     * key instead of committing on a pause. select() is the portable primitive
     * for a tty read with a deadline. */
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(t->tty_fd, &rfds);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int sr = select(t->tty_fd + 1, &rfds, NULL, NULL, &tv);
        if (sr == 0)
            return PK_TIMEOUT;
        if (sr < 0) {
            if (errno == EINTR) {
                if (paige_resized) {
                    paige_resized = 0;
                    paige_term_size(t);
                    return PK_RESIZE;
                }
                continue;
            }
            return PK_QUIT;
        }
        unsigned char c;
        ssize_t r = read(t->tty_fd, &c, 1);
        if (r <= 0)
            return PK_QUIT;
        return paige_term_decode_command_byte(t, c);
    }
}
