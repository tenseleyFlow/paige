/*
 * pty_test — drive paige-demo inside a real pseudo-terminal.
 *
 * forkpty() gives the child a controlling tty (so /dev/tty and the alternate
 * screen work); we feed keystrokes to the master and assert on the rendered
 * screen. This is how an interactive pager gets tested.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <termios.h>
#if defined(__linux__)
#include <pty.h>
#elif defined(__APPLE__)
#include <util.h>
#else
#include <libutil.h> /* BSD: needs struct winsize from above */
#endif

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static void on_alarm(int sig)
{
    (void)sig;
    const char *m = "FAIL: pty_test timed out\n";
    (void)!write(2, m, strlen(m));
    _exit(2);
}

/* Read from fd until it goes idle for idle_ms; NUL-terminate. */
static void read_screen(int fd, char *buf, size_t cap, int idle_ms)
{
    size_t len = 0;
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
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

static int has(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

int main(void)
{
    signal(SIGALRM, on_alarm);
    alarm(20); /* never hang CI */

    /* Shorten the digit-goto entry timeout so the goto tests stay fast and the
     * pause/accumulate margins are robust across slow and fast machines. */
    setenv("PAIGE_GOTO_MS", "150", 1);

    char tmpl[] = "/tmp/paige_pty_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    for (int i = 1; i <= 100; i++) {
        char line[32];
        int m = snprintf(line, sizeof line, "line%03d\n", i);
        (void)!write(fd, line, (size_t)m);
    }
    close(fd);

    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = 10;
    ws.ws_col = 40;

    int master;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) {
        perror("forkpty");
        unlink(tmpl);
        return 1;
    }
    if (pid == 0) {
        execl("./paige-demo", "paige-demo", tmpl, (char *)NULL);
        _exit(127);
    }

    char buf[1 << 16];
    int fails = 0;

    read_screen(master, buf, sizeof buf, 300); /* initial screen */
    if (!has(buf, "line001") || !has(buf, "line009")) {
        printf("FAIL: initial screen missing lines 1-9\n");
        fails++;
    }
    if (has(buf, "line020")) {
        printf("FAIL: showed more than a screenful\n");
        fails++;
    }

    (void)!write(master, "j", 1); /* scroll down one */
    read_screen(master, buf, sizeof buf, 300);
    if (!has(buf, "line002") || !has(buf, "line010")) {
        printf("FAIL: after 'j' expected lines 2-10\n");
        fails++;
    }

    (void)!write(master, "G", 1); /* jump to bottom */
    read_screen(master, buf, sizeof buf, 300);
    if (!has(buf, "line100")) {
        printf("FAIL: 'G' did not reach the last line\n");
        fails++;
    }

    (void)!write(master, "g", 1); /* jump to top */
    read_screen(master, buf, sizeof buf, 300);
    if (!has(buf, "line001")) {
        printf("FAIL: 'g' did not return to the top\n");
        fails++;
    }

    /* Live incremental goto. PAIGE_GOTO_MS (above) shortens the entry timeout
     * for speed, but the pauses below are sized past the 600ms DEFAULT so the
     * test is correct even where that env var does not take effect. The
     * accumulate case feeds both digits in one write, so the second digit beats
     * the timeout regardless of scheduling. */
    (void)!write(master, "16", 2);
    read_screen(master, buf, sizeof buf, 300);
    if (!has(buf, "line016") || !has(buf, "line024")) {
        printf("FAIL: '16' did not jump to line 16\n");
        fails++;
    }
    if (has(buf, "line030")) {
        printf("FAIL: '16' overshot\n");
        fails++;
    }
    usleep(800 * 1000);                        /* > default timeout: commit */
    read_screen(master, buf, sizeof buf, 200); /* drain to a clean buffer */

    /* a pause longer than the timeout commits the first number and starts a new
     * one: "1" <pause> "6" lands on line 6, not line 16. */
    (void)!write(master, "1", 1);
    read_screen(master, buf, sizeof buf, 200);
    usleep(800 * 1000); /* exceed the entry timeout: commit "1" */
    (void)!write(master, "6", 1);
    read_screen(master, buf, sizeof buf, 300);
    if (!has(buf, "line006") || !has(buf, "line014")) {
        printf("FAIL: paused '1..6' should land on line 6\n");
        fails++;
    }
    if (has(buf, "line016")) {
        printf("FAIL: paused '1..6' wrongly accumulated to 16\n");
        fails++;
    }
    usleep(800 * 1000); /* commit before the quit test */
    read_screen(master, buf, sizeof buf, 200);

    (void)!write(master, "q", 1); /* quit */
    /* Drain remaining output so the child never blocks on a full pty buffer. */
    read_screen(master, buf, sizeof buf, 300);
    int status;
    if (waitpid(pid, &status, WNOHANG) == 0) {
        /* still alive: give it a moment, then force it down (backstop) */
        read_screen(master, buf, sizeof buf, 300);
        if (waitpid(pid, &status, WNOHANG) == 0) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
        }
    }
    close(master);
    unlink(tmpl);

    if (fails == 0) {
        printf("pty: pager nav (j/G/g/q, goto-digits, quit-if-one-screen) OK\n");
        return 0;
    }
    return 1;
}
