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
    int digit; /* 0..9 when the last key was PK_DIGIT */
    unsigned char ch; /* printable byte when the last key was PK_CHAR */
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
    PK_LEFT,
    PK_RIGHT,
    PK_HOME,
    PK_END,
    PK_ENTER,
    PK_ESC,
    PK_BACKSPACE,
    PK_DELETE,
    PK_CHAR,  /* printable byte was typed; value in t->ch */
    PK_DIGIT, /* a digit was typed; value in t->digit */
    PK_RESIZE,
    PK_TIMEOUT, /* paige_term_key_timed: no key within the deadline */
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

/* Block for one key in text-entry mode. Printable bytes return PK_CHAR instead
 * of viewing commands, while arrows/edit keys keep semantic actions. */
int paige_term_key_input(struct paige_term *t);

/* Like paige_term_key but wait at most timeout_ms; PK_TIMEOUT if none arrives.
 * Used for the live digit-goto, where a pause ends number entry. */
int paige_term_key_timed(struct paige_term *t, int timeout_ms);

/* Pure single-byte decoders for tests and future modal input code. Do not pass
 * ESC here unless the term has a live tty fd for the rest of the sequence. */
int paige_term_decode_command_byte(struct paige_term *t, unsigned char c);
int paige_term_decode_input_byte(struct paige_term *t, unsigned char c);

#endif /* PAIGE_TERM_H */
