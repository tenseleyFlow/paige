#include "term.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* SIGWINCH sets this; the key loop turns it into PK_RESIZE. */
static volatile sig_atomic_t paige_resized = 0;

static void on_winch(int sig)
{
    (void)sig;
    paige_resized = 1;
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
        raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
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

/* Decode an ESC [ ... sequence into an action. */
static int decode_escape(struct paige_term *t)
{
    int b = read_byte(t);
    if (b != '[' && b != 'O')
        return PK_OTHER;
    int c = read_byte(t);
    switch (c) {
    case 'A':
        return PK_UP;
    case 'B':
        return PK_DOWN;
    case 'H':
        return PK_TOP;
    case 'F':
        return PK_BOTTOM;
    case '5':
        read_byte(t); /* consume '~' */
        return PK_PGUP;
    case '6':
        read_byte(t);
        return PK_PGDN;
    default:
        return PK_OTHER;
    }
}

/* Map one input byte to an action. */
static int decode_byte(struct paige_term *t, unsigned char c)
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
        return decode_escape(t);
    default:
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
        return decode_byte(t, c);
    }
}

int paige_term_key_timed(struct paige_term *t, int timeout_ms)
{
    for (;;) {
        struct pollfd p = { t->tty_fd, POLLIN, 0 };
        int pr = poll(&p, 1, timeout_ms);
        if (pr == 0)
            return PK_TIMEOUT;
        if (pr < 0) {
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
        return decode_byte(t, c);
    }
}
