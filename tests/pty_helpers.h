#ifndef PAIGE_PTY_HELPERS_H
#define PAIGE_PTY_HELPERS_H

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void pty_send(int fd, const char *bytes, size_t len)
{
    (void)!write(fd, bytes, len);
}

static void pty_send_text(int fd, const char *bytes)
{
    pty_send(fd, bytes, strlen(bytes));
}

/* Read from fd until it goes idle for idle_ms; NUL-terminate. */
static void pty_read_screen(int fd, char *buf, size_t cap, int idle_ms)
{
    size_t len = 0;
    for (;;) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, idle_ms) <= 0)
            break;
        ssize_t n = read(fd, buf + len, cap - len - 1);
        if (n <= 0)
            break;
        len += (size_t)n;
        if (len >= cap - 1)
            break;
    }
    buf[len] = '\0';
}

static void pty_drain(int fd, char *buf, size_t cap, int idle_ms)
{
    pty_read_screen(fd, buf, cap, idle_ms);
}

/*
 * Read from fd, accumulating into buf, until `needle` appears or budget_ms
 * elapses; NUL-terminate. Returns 1 if it appeared, 0 on timeout. Waiting for
 * the *expected* output instead of a fixed idle window means a slow, cold, or
 * loaded box just takes longer rather than racing the read and capturing an
 * empty/partial screen (the fixed-idle pty_read_screen flake).
 */
static int pty_wait_for(int fd, char *buf, size_t cap, const char *needle,
                        int budget_ms)
{
    size_t len = 0;
    int waited = 0;
    const int slice = 50;
    buf[0] = '\0';
    for (;;) {
        if (strstr(buf, needle))
            return 1;
        struct pollfd p = {fd, POLLIN, 0};
        int r = poll(&p, 1, slice);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (r == 0) {
            waited += slice;
            if (waited >= budget_ms)
                return 0;
            continue;
        }
        ssize_t n = read(fd, buf + len, cap - len - 1);
        if (n <= 0)
            return strstr(buf, needle) != NULL;
        len += (size_t)n;
        buf[len] = '\0';
        if (len >= cap - 1)
            return strstr(buf, needle) != NULL;
    }
}

static int pty_has(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

/* On failure, report which line markers the captured screen actually holds. */
static void pty_dump_visible(const char *buf)
{
    printf("  visible lines:");
    for (int i = 1; i <= 120; i++) {
        char needle[16];
        snprintf(needle, sizeof needle, "line%03d", i);
        if (strstr(buf, needle))
            printf(" %d", i);
    }
    printf("\n");
}

#endif /* PAIGE_PTY_HELPERS_H */
