/*
 * term.h — terminal control for the pager: raw mode, alternate screen, size,
 * resize, and key decoding. All POSIX (termios + ANSI), portable across
 * Linux, macOS, and the BSDs.
 */
#ifndef PAIGE_TERM_H
#define PAIGE_TERM_H

#include <stdbool.h>
#include <termios.h>

struct paige_term {
    int tty_fd; /* /dev/tty, used for keyboard input */
    int out_fd; /* terminal output (stdout) */
    struct termios orig;
    bool raw;
    bool alt;
    int rows, cols;
};

/* Decoded key actions. */
enum paige_key {
    PK_NONE = 0,
    PK_QUIT,
    PK_UP,
    PK_DOWN,
    PK_PGUP,
    PK_PGDN,
    PK_HALFUP,
    PK_HALFDOWN,
    PK_TOP,
    PK_BOTTOM,
    PK_RESIZE,
    PK_OTHER
};

/* Open /dev/tty and query the terminal size. Returns false if there is no
 * controlling terminal (caller should fall back to plain output). */
bool paige_term_open(struct paige_term *t);

/* Enter raw mode + alternate screen + hidden cursor; install SIGWINCH. */
void paige_term_enter(struct paige_term *t);

/* Restore everything (idempotent; also safe from a signal handler path). */
void paige_term_leave(struct paige_term *t);

/* Refresh rows/cols from the terminal. */
void paige_term_size(struct paige_term *t);

/* Block for one key, returning a decoded action (PK_RESIZE on a window
 * change). */
int paige_term_key(struct paige_term *t);

#endif /* PAIGE_TERM_H */
