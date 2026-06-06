/*
 * pty_test — drive paige-demo inside a real pseudo-terminal.
 *
 * forkpty() gives the child a controlling tty (so /dev/tty and the alternate
 * screen work); we feed keystrokes to the master and assert on the rendered
 * screen. This is how an interactive pager gets tested.
 *
 * Reads wait for the expected token (pty_wait_for) rather than a fixed idle
 * window, so a slow/loaded/cold box takes longer instead of racing the read.
 * When there is no pty at all (a minimal chroot/container) we skip rather than
 * fail; PAIGE_TEST_STRICT=1 makes that fatal for CI, where a pty must exist.
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

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "pty_helpers.h"

#define WAIT_MS 4000 /* per-assertion ceiling; generous for slow/cold boxes */

static void on_alarm(int sig)
{
    (void)sig;
    const char *m = "FAIL: pty_test timed out\n";
    (void)!write(2, m, strlen(m));
    _exit(2);
}

/* Build an mkstemp template under $TMPDIR (or /tmp), so the test runs in build
 * sandboxes that point TMPDIR elsewhere or restrict /tmp. */
static void mk_tmpl(char *out, size_t cap, const char *stem)
{
    const char *d = getenv("TMPDIR");
    if (!d || !*d)
        d = "/tmp";
    snprintf(out, cap, "%s/%s", d, stem);
}

/* Pull a counter (e.g. "render=" or "search=") out of the demo's
 * "paige-stats: render=N frames=N ... search=N ..." line, or -1 if absent.
 * Used to assert an interaction stayed bounded. */
static long stat_field(const char *buf, const char *key)
{
    const char *p = strstr(buf, key);
    return p ? atol(p + strlen(key)) : -1;
}

static pid_t spawn_demo(int *master, struct winsize *ws, const char *path)
{
    pid_t pid = forkpty(master, NULL, NULL, ws);
    if (pid < 0) {
        /* No pty available (e.g. a chroot/container without /dev/pts). Not a
         * paige failure — skip, unless strict mode demands a pty (CI). */
        if (getenv("PAIGE_TEST_STRICT")) {
            fprintf(stderr, "FAIL: forkpty: %s (PAIGE_TEST_STRICT)\n",
                    strerror(errno));
            return -1;
        }
        printf("skip - pty unavailable (%s)\n", strerror(errno));
        exit(0);
    }
    if (pid == 0) {
        execl("./paige-demo", "paige-demo", path, (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* Spawn the demo on two files, for the multi-document (:n / :p) test. */
static pid_t spawn_demo2(int *master, struct winsize *ws, const char *p1,
                         const char *p2)
{
    pid_t pid = forkpty(master, NULL, NULL, ws);
    if (pid < 0) {
        if (getenv("PAIGE_TEST_STRICT")) {
            fprintf(stderr, "FAIL: forkpty: %s (PAIGE_TEST_STRICT)\n",
                    strerror(errno));
            return -1;
        }
        printf("skip - pty unavailable (%s)\n", strerror(errno));
        exit(0);
    }
    if (pid == 0) {
        execl("./paige-demo", "paige-demo", p1, p2, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static void finish_demo(int master, pid_t pid, char *buf, size_t cap,
                        int *status)
{
    if (waitpid(pid, status, WNOHANG) == 0) {
        pty_drain(master, buf, cap, 300);
        if (waitpid(pid, status, WNOHANG) == 0) {
            kill(pid, SIGTERM);
            waitpid(pid, status, 0);
        }
    }
    close(master);
}

int main(void)
{
    signal(SIGALRM, on_alarm);
    alarm(60); /* hard backstop against a hung demo; healthy runs are seconds */

    /* Shorten the digit-goto entry timeout so the goto tests stay fast and the
     * pause/accumulate margins are robust across slow and fast machines. */
    setenv("PAIGE_GOTO_MS", "150", 1);
    setenv("PAIGE_FOLLOW_MS", "80", 1);
    setenv("PAIGE_STATS", "1", 1);

    char tmpl[4096];
    mk_tmpl(tmpl, sizeof tmpl, "paige_pty_XXXXXX");
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    for (int i = 1; i <= 120; i++) {
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
    pid_t pid = spawn_demo(&master, &ws, tmpl);
    if (pid < 0) {
        unlink(tmpl);
        return 1;
    }

    char buf[1 << 16];
    int fails = 0;

    pty_wait_for(master, buf, sizeof buf, "line009", WAIT_MS); /* initial */
    if (!pty_has(buf, "line001") || !pty_has(buf, "line009")) {
        printf("FAIL: initial screen missing lines 1-9\n");
        fails++;
    }
    if (pty_has(buf, "line020")) {
        printf("FAIL: showed more than a screenful\n");
        fails++;
    }

    pty_send_text(master, "''"); /* no previous position yet */
    pty_wait_for(master, buf, sizeof buf, "no previous position", WAIT_MS);
    if (!pty_has(buf, "no previous position")) {
        printf("FAIL: previous-position mark should report no prior jump\n");
        pty_dump_visible(buf);
        fails++;
    }

    pty_send_text(master, "j"); /* scroll down one */
    pty_wait_for(master, buf, sizeof buf, "line010", WAIT_MS);
    if (!pty_has(buf, "line002") || !pty_has(buf, "line010")) {
        printf("FAIL: after 'j' expected lines 2-10\n");
        fails++;
    }

    pty_send_text(master, "G"); /* jump to bottom */
    pty_wait_for(master, buf, sizeof buf, "line120", WAIT_MS);
    if (!pty_has(buf, "line120")) {
        printf("FAIL: 'G' did not reach the last line\n");
        fails++;
    }

    pty_send_text(master, "g"); /* jump to top */
    pty_wait_for(master, buf, sizeof buf, "line001", WAIT_MS);
    if (!pty_has(buf, "line001")) {
        printf("FAIL: 'g' did not return to the top\n");
        fails++;
    }

    pty_send_text(master, "/line05\n"); /* forward search; line050..059 = 10 */
    pty_wait_for(master, buf, sizeof buf, "[1/10]", WAIT_MS);
    if (!pty_has(buf, "line 50")) {
        printf("FAIL: search for line05 did not land on line 50\n");
        pty_dump_visible(buf);
        fails++;
    }
    if (!pty_has(buf, "\x1b[7mline05")) {
        printf("FAIL: search highlight missing\n");
        fails++;
    }
    if (!pty_has(buf, "[1/10]")) {
        printf("FAIL: search overview did not show match 1 of 10\n");
        pty_dump_visible(buf);
        fails++;
    }
    if (!pty_has(buf, "searching")) {
        printf("FAIL: progressive 'searching...' status not shown\n");
        fails++;
    }

    pty_send_text(master, "n"); /* next match */
    pty_wait_for(master, buf, sizeof buf, "[2/10]", WAIT_MS);
    if (!pty_has(buf, "line 51") || !pty_has(buf, "[2/10]")) {
        printf("FAIL: 'n' did not advance to match 2 of 10\n");
        pty_dump_visible(buf);
        fails++;
    }

    pty_send_text(master, "N"); /* previous match */
    pty_wait_for(master, buf, sizeof buf, "[1/10]", WAIT_MS);
    if (!pty_has(buf, "line 50") || !pty_has(buf, "[1/10]")) {
        printf("FAIL: 'N' did not return to match 1 of 10\n");
        pty_dump_visible(buf);
        fails++;
    }

    pty_send_text(master, "?line02\n"); /* backward search */
    pty_wait_for(master, buf, sizeof buf, "line 29", WAIT_MS);
    if (!pty_has(buf, "line 29")) {
        printf("FAIL: backward search did not land on line 29\n");
        pty_dump_visible(buf);
        fails++;
    }

    pty_send_text(master, "/zzzz\n"); /* not found */
    pty_wait_for(master, buf, sizeof buf, "pattern not found", WAIT_MS);
    if (!pty_has(buf, "pattern not found") || !pty_has(buf, "line029")) {
        printf("FAIL: not-found search did not report and restore origin\n");
        pty_dump_visible(buf);
        fails++;
    }

    pty_send_text(master, "/line080"); /* live search, then cancel */
    pty_wait_for(master, buf, sizeof buf, "line080", WAIT_MS);
    if (!pty_has(buf, "line080")) {
        printf("FAIL: incremental search did not move to line 80\n");
        pty_dump_visible(buf);
        fails++;
    }
    pty_send(master, "\x1b", 1);
    pty_wait_for(master, buf, sizeof buf, "line029", WAIT_MS);
    if (!pty_has(buf, "line029")) {
        printf("FAIL: escape did not restore pre-search view\n");
        pty_dump_visible(buf);
        fails++;
    }

    pty_send_text(master, "50%"); /* percent goto on 120 known lines */
    pty_wait_for(master, buf, sizeof buf, "line 60", WAIT_MS);
    if (!pty_has(buf, "line060") || !pty_has(buf, "line 60")) {
        printf("FAIL: '50%%' did not jump to the middle of known content\n");
        pty_dump_visible(buf);
        fails++;
    }

    pty_send_text(master, "ma"); /* set mark a at line 60 */
    pty_wait_for(master, buf, sizeof buf, "mark a set", WAIT_MS);
    if (!pty_has(buf, "mark a set")) {
        printf("FAIL: mark set status missing\n");
        pty_dump_visible(buf);
        fails++;
    }
    pty_send_text(master, "g");
    pty_wait_for(master, buf, sizeof buf, "line001", WAIT_MS);
    if (!pty_has(buf, "line001")) {
        printf("FAIL: top jump before mark test failed\n");
        fails++;
    }
    pty_send_text(master, "'a");
    pty_wait_for(master, buf, sizeof buf, "line060", WAIT_MS);
    if (!pty_has(buf, "line060")) {
        printf("FAIL: mark jump did not return to line 60\n");
        pty_dump_visible(buf);
        fails++;
    }
    pty_send_text(master, "''");
    pty_wait_for(master, buf, sizeof buf, "line001", WAIT_MS);
    if (!pty_has(buf, "line001")) {
        printf("FAIL: previous-position mark did not return to line 1\n");
        pty_dump_visible(buf);
        fails++;
    }
    pty_send_text(master, "'z");
    pty_wait_for(master, buf, sizeof buf, "mark z not set", WAIT_MS);
    if (!pty_has(buf, "mark z not set")) {
        printf("FAIL: missing mark status not shown\n");
        pty_dump_visible(buf);
        fails++;
    }

    pty_send_text(master, "~");
    pty_wait_for(master, buf, sizeof buf, "unknown command", WAIT_MS);
    if (!pty_has(buf, "unknown command")) {
        printf("FAIL: unknown command status not shown\n");
        pty_dump_visible(buf);
        fails++;
    }

    pty_send_text(master, "h");
    pty_wait_for(master, buf, sizeof buf, "Navigation:", WAIT_MS);
    if (!pty_has(buf, "paige help") || !pty_has(buf, "Navigation:")) {
        printf("FAIL: help did not open\n");
        pty_dump_visible(buf);
        fails++;
    }
    pty_send_text(master, "q");
    pty_wait_for(master, buf, sizeof buf, "line001", WAIT_MS);
    if (!pty_has(buf, "line001") || pty_has(buf, "paige help")) {
        printf("FAIL: help did not return to saved view\n");
        pty_dump_visible(buf);
        fails++;
    }

    pty_send_text(master, "P");
    pty_wait_for(master, buf, sizeof buf, "terminal:", WAIT_MS);
    if (!pty_has(buf, "paige performance") || !pty_has(buf, "render:") ||
        !pty_has(buf, "terminal:")) {
        printf("FAIL: performance panel did not open\n");
        pty_dump_visible(buf);
        fails++;
    }
    pty_send_text(master, "q");
    pty_wait_for(master, buf, sizeof buf, "line001", WAIT_MS);
    if (!pty_has(buf, "line001") || pty_has(buf, "paige performance")) {
        printf("FAIL: performance panel did not return to saved view\n");
        pty_dump_visible(buf);
        fails++;
    }

    /* Live incremental goto. PAIGE_GOTO_MS (above) shortens the entry timeout
     * for speed, but the pauses below are sized past the 600ms DEFAULT so the
     * test is correct even where that env var does not take effect. The
     * accumulate case feeds both digits in one write, so the second digit beats
     * the timeout regardless of scheduling. */
    pty_send_text(master, "16");
    pty_wait_for(master, buf, sizeof buf, "line024", WAIT_MS);
    if (!pty_has(buf, "line016") || !pty_has(buf, "line024")) {
        printf("FAIL: '16' did not jump to line 16\n");
        fails++;
    }
    if (pty_has(buf, "line030")) {
        printf("FAIL: '16' overshot\n");
        fails++;
    }
    usleep(800 * 1000);                      /* > default timeout: commit */
    pty_drain(master, buf, sizeof buf, 200); /* drain to a clean buffer */

    /* a pause longer than the timeout commits the first number and starts a new
     * one: "1" <pause> "6" lands on line 6, not line 16. */
    pty_send_text(master, "1");
    pty_drain(master, buf, sizeof buf, 200);
    usleep(800 * 1000); /* exceed the entry timeout: commit "1" */
    pty_send_text(master, "6");
    pty_wait_for(master, buf, sizeof buf, "line014", WAIT_MS);
    if (!pty_has(buf, "line006") || !pty_has(buf, "line014")) {
        printf("FAIL: paused '1..6' should land on line 6\n");
        pty_dump_visible(buf);
        fails++;
    }
    if (pty_has(buf, "line016")) {
        printf("FAIL: paused '1..6' wrongly accumulated to 16\n");
        pty_dump_visible(buf);
        fails++;
    }
    usleep(800 * 1000); /* commit before the quit test */
    pty_drain(master, buf, sizeof buf, 200);

    pty_send_text(master, "q"); /* quit */
    pty_wait_for(master, buf, sizeof buf, "paige-stats:", WAIT_MS);
    if (!pty_has(buf, "paige-stats:")) {
        printf("FAIL: stats output missing after quit\n");
        fails++;
    }
    int status;
    finish_demo(master, pid, buf, sizeof buf, &status);

    /* Ctrl-C is delivered as a byte (ISIG is cleared) and quits cleanly,
     * restoring the terminal (leaving the alternate screen). */
    pid = spawn_demo(&master, &ws, tmpl);
    if (pid < 0) {
        unlink(tmpl);
        return 1;
    }
    pty_wait_for(master, buf, sizeof buf, "line001", WAIT_MS);
    pty_send(master, "\x03", 1); /* Ctrl-C */
    pty_drain(master, buf, sizeof buf, 300);
    if (!pty_has(buf, "\x1b[?1049l")) {
        printf("FAIL: Ctrl-C did not restore the terminal\n");
        fails++;
    }
    finish_demo(master, pid, buf, sizeof buf, &status);
    if (!WIFEXITED(status)) {
        printf("FAIL: Ctrl-C did not exit cleanly\n");
        fails++;
    }

    /* An external SIGTERM must also restore the terminal before dying. */
    pid = spawn_demo(&master, &ws, tmpl);
    if (pid < 0) {
        unlink(tmpl);
        return 1;
    }
    pty_wait_for(master, buf, sizeof buf, "line001", WAIT_MS);
    kill(pid, SIGTERM);
    pty_drain(master, buf, sizeof buf, 500);
    if (!pty_has(buf, "\x1b[?1049l")) {
        printf("FAIL: SIGTERM did not restore the terminal\n");
        fails++;
    }
    waitpid(pid, &status, 0);
    close(master);
    if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM)) {
        printf("FAIL: process did not terminate via SIGTERM\n");
        fails++;
    }

    /* SIGTERM must kill us even when the tty is WEDGED — output buffer full
     * with no reader (an orphaned pty / disconnected terminal). The fatal
     * handler's restore must not block (non-blocking write + TCSANOW);
     * otherwise the process becomes un-SIGTERM-able and only `kill -9` works.
     *
     * Skipped on macOS: its pseudo-terminal flow-control makes "wedge the
     * output buffer" unreliable to reproduce in a test harness. The fix is
     * portable POSIX (verified on Linux + FreeBSD CI and locally). */
#if !defined(__APPLE__)
    pid = spawn_demo(&master, &ws, tmpl);
    if (pid < 0) {
        unlink(tmpl);
        return 1;
    }
    pty_wait_for(master, buf, sizeof buf, "line001", WAIT_MS);
    /* A separate flooder process feeds input without anyone reading the demo's
     * output, so the demo renders frame after frame until its output write
     * blocks on the full pty buffer — a wedged tty. The flooder's own writes
     * may block once the input buffer fills; that's fine because it is a child
     * we SIGKILL. Decoupling the flood keeps the TEST from ever blocking,
     * portably (pty O_NONBLOCK / select-for-write are unreliable across
     * macOS/BSD). */
    pid_t flooder = fork();
    if (flooder == 0) {
        for (;;)
            if (write(master, "j", 1) != 1)
                _exit(0);
    }
    usleep(400000); /* let the demo wedge on output */
    kill(pid, SIGTERM);
    /* Bounded wait — never block forever, or a regression hangs the suite. */
    {
        int died = 0;
        for (int i = 0; i < 200; i++) { /* up to ~4s */
            if (waitpid(pid, &status, WNOHANG) == pid) {
                died = 1;
                break;
            }
            usleep(20000);
        }
        if (!died) {
            printf("FAIL: SIGTERM did not kill a demo on a wedged tty "
                   "(un-SIGTERM-able)\n");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            fails++;
        } else if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM)) {
            printf("FAIL: wedged demo did not terminate via SIGTERM\n");
            fails++;
        }
    }
    if (flooder > 0) {
        kill(flooder, SIGKILL);
        waitpid(flooder, NULL, 0);
    }
    close(master);
#endif /* !__APPLE__ */

    setenv("PAIGE_NO_RAW", "1", 1);
    pid = spawn_demo(&master, &ws, tmpl);
    if (pid < 0) {
        unlink(tmpl);
        return 1;
    }
    pty_wait_for(master, buf, sizeof buf, "line001", WAIT_MS);
    pty_send_text(master, "/line050\n");
    pty_wait_for(master, buf, sizeof buf, "search unavailable", WAIT_MS);
    if (!pty_has(buf, "search unavailable")) {
        printf(
            "FAIL: missing raw-line hook should report search unavailable\n");
        fails++;
    }
    pty_send_text(master, "q");
    finish_demo(master, pid, buf, sizeof buf, &status);

    unsetenv("PAIGE_NO_RAW");
    setenv("PAIGE_NO_COUNT", "1", 1);
    pid = spawn_demo(&master, &ws, tmpl);
    if (pid < 0) {
        unlink(tmpl);
        return 1;
    }
    pty_wait_for(master, buf, sizeof buf, "line001", WAIT_MS);
    pty_send_text(master, "50%");
    pty_wait_for(master, buf, sizeof buf, "percent unavailable", WAIT_MS);
    if (!pty_has(buf, "percent unavailable") || !pty_has(buf, "line001")) {
        printf("FAIL: missing line-count hook should report percent "
               "unavailable\n");
        pty_dump_visible(buf);
        fails++;
    }
    pty_send_text(master, "q");
    finish_demo(master, pid, buf, sizeof buf, &status);

    unsetenv("PAIGE_NO_COUNT");
    char follow_tmpl[4096];
    mk_tmpl(follow_tmpl, sizeof follow_tmpl, "paige_follow_XXXXXX");
    int ffd = mkstemp(follow_tmpl);
    if (ffd < 0) {
        perror("mkstemp");
        unlink(tmpl);
        return 1;
    }
    for (int i = 1; i <= 11; i++) {
        char line[32];
        int m = snprintf(line, sizeof line, "follow%03d\n", i);
        (void)!write(ffd, line, (size_t)m);
    }
    (void)!write(ffd, "partial", 7);
    close(ffd);

    pid = spawn_demo(&master, &ws, follow_tmpl);
    if (pid < 0) {
        unlink(tmpl);
        unlink(follow_tmpl);
        return 1;
    }
    pty_wait_for(master, buf, sizeof buf, "follow001", WAIT_MS);
    pty_send_text(master, "F");
    pty_drain(master, buf, sizeof buf, 300); /* enter follow mode */
    ffd = open(follow_tmpl, O_WRONLY | O_APPEND);
    if (ffd < 0) {
        perror("open follow append");
        fails++;
    } else {
        const char *append = "-more\nfollow-new\n";
        (void)!write(ffd, append, strlen(append));
        close(ffd);
    }
    pty_wait_for(master, buf, sizeof buf, "follow-new", WAIT_MS);
    if (!pty_has(buf, "partial-more") || !pty_has(buf, "follow-new") ||
        !pty_has(buf, "FOLLOW")) {
        printf("FAIL: follow mode did not redraw appended content\n");
        pty_dump_visible(buf);
        fails++;
    }
    /* the freshly-appended line is highlighted (bold). */
    if (!pty_has(buf, "\x1b[1mfollow-new")) {
        printf("FAIL: appended follow line was not highlighted\n");
        pty_dump_visible(buf);
        fails++;
    }
    pty_send_text(master, "k"); /* manual navigation pauses follow */
    pty_drain(master, buf, sizeof buf, 300);
    ffd = open(follow_tmpl, O_WRONLY | O_APPEND);
    if (ffd < 0) {
        perror("open follow paused append");
        fails++;
    } else {
        const char *append = "paused-new\n";
        (void)!write(ffd, append, strlen(append));
        close(ffd);
    }
    /* Paused: nothing should redraw. Idle-drain (returns fast if no output),
     * with a margin well past the follow poll interval. */
    pty_read_screen(master, buf, sizeof buf, 400);
    if (pty_has(buf, "paused-new")) {
        printf("FAIL: paused follow should not redraw appended content\n");
        fails++;
    }
    pty_send_text(master, "F");
    pty_wait_for(master, buf, sizeof buf, "paused-new", WAIT_MS);
    if (!pty_has(buf, "paused-new") || !pty_has(buf, "FOLLOW")) {
        printf("FAIL: follow resume did not load paused append\n");
        pty_dump_visible(buf);
        fails++;
    }
    pty_send_text(master, "q");
    finish_demo(master, pid, buf, sizeof buf, &status);

    setenv("PAIGE_CHOP", "1", 1);
    char chop_tmpl[4096];
    mk_tmpl(chop_tmpl, sizeof chop_tmpl, "paige_chop_XXXXXX");
    int cfd = mkstemp(chop_tmpl);
    if (cfd < 0) {
        perror("mkstemp");
        unlink(tmpl);
        return 1;
    }
    const char *prefix =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-END";
    (void)!write(cfd, prefix, strlen(prefix));
    for (int i = 0; i < 500; i++)
        (void)!write(cfd, "X", 1);
    const char *tail = "\nsecond-line\nthird-line\n";
    (void)!write(cfd, tail, strlen(tail));
    close(cfd);

    pid = spawn_demo(&master, &ws, chop_tmpl);
    if (pid < 0) {
        unlink(tmpl);
        unlink(chop_tmpl);
        return 1;
    }
    pty_wait_for(master, buf, sizeof buf, "second-line", WAIT_MS);
    if (!pty_has(buf, "second-line") || !pty_has(buf, ">")) {
        printf("FAIL: chop mode should show one row per logical line\n");
        fails++;
    }
    /* long-line focus: a column ruler row and a col-range/length readout. */
    pty_send_text(master, "|"); /* toggle the ruler */
    pty_wait_for(master, buf, sizeof buf, "....+....|", WAIT_MS);
    if (!pty_has(buf, "....+....|") || !pty_has(buf, "col 1-")) {
        printf("FAIL: column ruler / readout missing in chop mode\n");
        pty_dump_visible(buf);
        fails++;
    }
    pty_send_text(master, "|"); /* toggle off */
    pty_drain(master, buf, sizeof buf, 150);
    pty_send(master, "\x1b[C", 3);
    pty_wait_for(master, buf, sizeof buf, "col 9", WAIT_MS);
    if (!pty_has(buf, "col 9") || !pty_has(buf, "<")) {
        printf("FAIL: right arrow did not horizontally scroll\n");
        fails++;
    }
    pty_send(master, "\x1b[D", 3);
    pty_wait_for(master, buf, sizeof buf, "col 1", WAIT_MS);
    if (!pty_has(buf, "col 1")) {
        printf("FAIL: left arrow did not return to column 1\n");
        fails++;
    }
    pty_send_text(master, "/xyz-END\n");
    pty_wait_for(master, buf, sizeof buf, "col 29", WAIT_MS);
    if (!pty_has(buf, "col 29") || !pty_has(buf, "\x1b[7mxyz-END")) {
        printf("FAIL: chop search did not reveal off-screen match\n");
        fails++;
    }
    pty_send_text(master, "q");
    finish_demo(master, pid, buf, sizeof buf, &status);
    /* On a large known-length doc: G stays O(screen) (goto_bottom was
     * O(document)), and incremental search stays bounded per keystroke (it did
     * a full forward+wraparound scan on every character typed). */
    char big_tmpl[4096];
    mk_tmpl(big_tmpl, sizeof big_tmpl, "paige_big_XXXXXX");
    int bfd = mkstemp(big_tmpl);
    if (bfd < 0) {
        perror("mkstemp");
        unlink(tmpl);
        unlink(follow_tmpl);
        unlink(chop_tmpl);
        return 1;
    }
    enum { BIG_LINES = 100000 };
    for (int i = 1; i <= BIG_LINES; i++) {
        char line[16];
        int m = snprintf(line, sizeof line, "L%06d\n", i);
        (void)!write(bfd, line, (size_t)m);
    }
    close(bfd);
    pid = spawn_demo(&master, &ws, big_tmpl);
    if (pid < 0) {
        unlink(tmpl);
        unlink(follow_tmpl);
        unlink(chop_tmpl);
        unlink(big_tmpl);
        return 1;
    }
    pty_wait_for(master, buf, sizeof buf, "L000001", WAIT_MS);
    pty_send_text(master, "G");
    pty_wait_for(master, buf, sizeof buf, "L100000", WAIT_MS);
    /* Back to the top, then type a never-matching pattern one char at a time.
     * Each keystroke must scan at most the preview window, not the whole file.
     */
    pty_send_text(master, "g");
    pty_wait_for(master, buf, sizeof buf, "L000001", WAIT_MS);
    pty_send_text(master, "/zzzzzzzz"); /* 8 chars, no Enter */
    pty_drain(master, buf, sizeof buf, 300);
    pty_send(master, "\x1b", 1); /* cancel the search */
    pty_wait_for(master, buf, sizeof buf, "L000001", WAIT_MS);
    pty_send_text(master, "q");
    pty_wait_for(master, buf, sizeof buf, "paige-stats:", WAIT_MS);
    long rc = stat_field(buf, "render=");
    long sl = stat_field(buf, "search_lines=");
    if (rc < 0 || rc > 1000) {
        printf("FAIL: G rendered %ld lines on a %d-line doc (expected "
               "O(screen), not O(document))\n",
               rc, BIG_LINES);
        fails++;
    }
    /* 8 keystrokes * 10000-line preview window = ~80k; an unbounded
     * per-keystroke forward+wrap scan would be ~1.6M. */
    if (sl < 0 || sl > 250000) {
        printf("FAIL: incremental search scanned %ld lines on a %d-line doc "
               "(expected bounded per keystroke)\n",
               sl, BIG_LINES);
        fails++;
    }
    finish_demo(master, pid, buf, sizeof buf, &status);

    /* Same O(screen) G WITHOUT a line_count: a seekable host that implements
     * seek_end reaches the bottom just as cheaply. This is the path that
     * matters for the production host (mat omits line_count), so gate it the
     * same way. */
    setenv("PAIGE_NO_COUNT", "1", 1);
    pid = spawn_demo(&master, &ws, big_tmpl);
    if (pid < 0) {
        unlink(tmpl);
        unlink(follow_tmpl);
        unlink(chop_tmpl);
        unlink(big_tmpl);
        return 1;
    }
    pty_wait_for(master, buf, sizeof buf, "L000001", WAIT_MS);
    pty_send_text(master, "G");
    pty_wait_for(master, buf, sizeof buf, "L100000", WAIT_MS);
    pty_send_text(master, "q");
    pty_wait_for(master, buf, sizeof buf, "paige-stats:", WAIT_MS);
    long rc_se = stat_field(buf, "render=");
    if (rc_se < 0 || rc_se > 1000) {
        printf("FAIL: G via seek_end rendered %ld lines on a %d-line doc "
               "(expected O(screen))\n",
               rc_se, BIG_LINES);
        fails++;
    }
    finish_demo(master, pid, buf, sizeof buf, &status);

    /* With neither line_count nor seek_end, G falls back to the forward scan:
     * it is no longer O(screen) but must still functionally reach the last
     * line. */
    setenv("PAIGE_NO_SEEK_END", "1", 1);
    pid = spawn_demo(&master, &ws, big_tmpl);
    if (pid < 0) {
        unlink(tmpl);
        unlink(follow_tmpl);
        unlink(chop_tmpl);
        unlink(big_tmpl);
        return 1;
    }
    pty_wait_for(master, buf, sizeof buf, "L000001", WAIT_MS);
    pty_send_text(master, "G");
    pty_wait_for(master, buf, sizeof buf, "L100000", WAIT_MS);
    if (!pty_has(buf, "L100000")) {
        printf("FAIL: forward-scan fallback G did not reach the last line\n");
        fails++;
    }
    pty_send_text(master, "q");
    finish_demo(master, pid, buf, sizeof buf, &status);
    unsetenv("PAIGE_NO_SEEK_END");
    unsetenv("PAIGE_NO_COUNT");

    /* A long unbounded search (committed with Enter) can be cancelled by a
     * keypress: send the pattern, Enter, and one extra byte in a single write.
     * The byte sits buffered; when the scan reaches its interrupt checkpoint it
     * finds it, consumes it, and reports "search interrupted". */
    pid = spawn_demo(&master, &ws, big_tmpl);
    if (pid < 0) {
        unlink(tmpl);
        unlink(follow_tmpl);
        unlink(chop_tmpl);
        unlink(big_tmpl);
        return 1;
    }
    pty_wait_for(master, buf, sizeof buf, "L000001", WAIT_MS);
    pty_send_text(master, "/zzzzzzzz\nx");
    pty_wait_for(master, buf, sizeof buf, "search interrupted", WAIT_MS);
    if (!pty_has(buf, "search interrupted")) {
        printf("FAIL: a long search was not cancelled by a keypress\n");
        pty_dump_visible(buf);
        fails++;
    }
    pty_send_text(master, "q");
    finish_demo(master, pid, buf, sizeof buf, &status);
    unlink(big_tmpl);

    /* A very long line in WRAP mode must materialize only the visible segment
     * window, not every segment of the line (the old hidden O(line) cost). */
    unsetenv("PAIGE_CHOP"); /* the chop test above set it */
    char wrap_tmpl[4096];
    mk_tmpl(wrap_tmpl, sizeof wrap_tmpl, "paige_wrap_XXXXXX");
    int wfd = mkstemp(wrap_tmpl);
    if (wfd < 0) {
        perror("mkstemp");
        unlink(tmpl);
        unlink(follow_tmpl);
        unlink(chop_tmpl);
        return 1;
    }
    {
        char chunk[4096];
        memset(chunk, 'W', sizeof chunk);
        size_t left = 1000000; /* ~25000 segments at width 40 */
        while (left > 0) {
            size_t k = left < sizeof chunk ? left : sizeof chunk;
            (void)!write(wfd, chunk, k);
            left -= k;
        }
        (void)!write(wfd, "\n", 1);
    }
    close(wfd);
    pid = spawn_demo(&master, &ws, wrap_tmpl);
    if (pid < 0) {
        unlink(tmpl);
        unlink(follow_tmpl);
        unlink(chop_tmpl);
        unlink(wrap_tmpl);
        return 1;
    }
    pty_wait_for(master, buf, sizeof buf, "WWWW", WAIT_MS);
    pty_send_text(master, "jjj"); /* scroll within the long wrapped line */
    pty_drain(master, buf, sizeof buf, 300);
    pty_send_text(master, "q");
    pty_wait_for(master, buf, sizeof buf, "paige-stats:", WAIT_MS);
    long segs = stat_field(buf, "segments=");
    if (segs < 0 || segs > 1000) {
        printf("FAIL: wrap-mode long line materialized %ld segments "
               "(expected O(visible), not O(line))\n",
               segs);
        fails++;
    }
    finish_demo(master, pid, buf, sizeof buf, &status);
    unlink(wrap_tmpl);

    /* SIGWINCH: shrinking the terminal must reflow without a keypress. */
    pid = spawn_demo(&master, &ws, tmpl);
    if (pid < 0) {
        unlink(tmpl);
        unlink(follow_tmpl);
        unlink(chop_tmpl);
        return 1;
    }
    pty_wait_for(master, buf, sizeof buf, "line009", WAIT_MS);
    pty_drain(master, buf, sizeof buf,
              200); /* consume the rest of the screen */
    struct winsize small = ws;
    small.ws_row = 5; /* body shrinks 9 -> 4 rows */
    if (ioctl(master, TIOCSWINSZ, &small) != 0)
        perror("TIOCSWINSZ");
    pty_wait_for(master, buf, sizeof buf, "line004",
                 WAIT_MS); /* fresh redraw */
    if (!pty_has(buf, "line001") || pty_has(buf, "line009")) {
        printf("FAIL: SIGWINCH resize did not reflow to fewer rows\n");
        pty_dump_visible(buf);
        fails++;
    }
    pty_send_text(master, "q");
    finish_demo(master, pid, buf, sizeof buf, &status);

    /* A forward search with no match below the cursor must wrap and say so. */
    pid = spawn_demo(&master, &ws, tmpl);
    if (pid < 0) {
        unlink(tmpl);
        unlink(follow_tmpl);
        unlink(chop_tmpl);
        return 1;
    }
    pty_wait_for(master, buf, sizeof buf, "line009", WAIT_MS);
    pty_send_text(master, "G");
    pty_wait_for(master, buf, sizeof buf, "line120", WAIT_MS);
    pty_drain(master, buf, sizeof buf, 150); /* settle before the search */
    pty_send_text(master, "/line005\n");     /* only matches above -> wraps */
    pty_wait_for(master, buf, sizeof buf, "search wrapped", WAIT_MS);
    if (!pty_has(buf, "search wrapped") || !pty_has(buf, "line005")) {
        printf("FAIL: forward search past EOF did not wrap to line 5\n");
        pty_dump_visible(buf);
        fails++;
    }
    pty_send_text(master, "q");
    finish_demo(master, pid, buf, sizeof buf, &status);

    /* Empty file: the demo's quit_if_one_screen prints nothing and exits 0
     * (a one-line empty doc fits a screen); it must not crash or hang. */
    char empty_tmpl[4096];
    mk_tmpl(empty_tmpl, sizeof empty_tmpl, "paige_empty_XXXXXX");
    int efd = mkstemp(empty_tmpl);
    if (efd < 0) {
        perror("mkstemp");
        unlink(tmpl);
        unlink(follow_tmpl);
        unlink(chop_tmpl);
        return 1;
    }
    close(efd); /* leave it 0 bytes */
    pid = spawn_demo(&master, &ws, empty_tmpl);
    if (pid < 0) {
        unlink(tmpl);
        unlink(follow_tmpl);
        unlink(chop_tmpl);
        unlink(empty_tmpl);
        return 1;
    }
    finish_demo(master, pid, buf, sizeof buf, &status);
    if (!WIFEXITED(status)) {
        printf("FAIL: empty-file pager did not exit cleanly\n");
        fails++;
    }
    unlink(empty_tmpl);

    /* Semantic jumps: ] / [ move to the next/prev host landmark (ERROR lines).
     */
    char lm_tmpl[4096];
    mk_tmpl(lm_tmpl, sizeof lm_tmpl, "paige_lm_XXXXXX");
    int lfd = mkstemp(lm_tmpl);
    if (lfd < 0) {
        perror("mkstemp");
        unlink(tmpl);
        unlink(follow_tmpl);
        unlink(chop_tmpl);
        return 1;
    }
    for (int i = 1; i <= 30; i++) {
        char line[40];
        int m;
        if (i == 10)
            m = snprintf(line, sizeof line, "ERROR alpha at %d\n", i);
        else if (i == 20)
            m = snprintf(line, sizeof line, "ERROR beta at %d\n", i);
        else
            m = snprintf(line, sizeof line, "row%02d\n", i);
        (void)!write(lfd, line, (size_t)m);
    }
    close(lfd);
    pid = spawn_demo(&master, &ws, lm_tmpl);
    if (pid < 0) {
        unlink(tmpl);
        unlink(follow_tmpl);
        unlink(chop_tmpl);
        unlink(lm_tmpl);
        return 1;
    }
    pty_wait_for(master, buf, sizeof buf, "row01", WAIT_MS);
    pty_send_text(master, "]"); /* next landmark */
    pty_wait_for(master, buf, sizeof buf, "next landmark", WAIT_MS);
    if (!pty_has(buf, "ERROR alpha") || !pty_has(buf, "next landmark")) {
        printf("FAIL: ] did not jump to the next landmark\n");
        fails++;
    }
    pty_send_text(master, "]"); /* next again */
    pty_wait_for(master, buf, sizeof buf, "ERROR beta", WAIT_MS);
    if (!pty_has(buf, "ERROR beta")) {
        printf("FAIL: second ] did not advance to the next landmark\n");
        fails++;
    }
    pty_send_text(master, "["); /* previous landmark */
    pty_wait_for(master, buf, sizeof buf, "previous landmark", WAIT_MS);
    if (!pty_has(buf, "ERROR alpha") || !pty_has(buf, "previous landmark")) {
        printf("FAIL: [ did not jump to the previous landmark\n");
        fails++;
    }
    pty_send_text(master, "q");
    finish_demo(master, pid, buf, sizeof buf, &status);
    unlink(lm_tmpl);

    /* Reversible filter: search, then & to page only the matching lines. */
    pid = spawn_demo(&master, &ws, tmpl);
    if (pid < 0) {
        unlink(tmpl);
        unlink(follow_tmpl);
        unlink(chop_tmpl);
        return 1;
    }
    pty_wait_for(master, buf, sizeof buf, "line009", WAIT_MS);
    pty_send_text(master, "/line05\n"); /* 10 matches: line050..line059 */
    pty_wait_for(master, buf, sizeof buf, "[1/10]", WAIT_MS);
    pty_send_text(master, "&"); /* filter to those lines */
    pty_wait_for(master, buf, sizeof buf, "filtered", WAIT_MS);
    /* the filtered view shows only matching lines (highlighted), not line001.
     */
    if (!pty_has(buf, "\x1b[7mline05") || pty_has(buf, "line001") ||
        !pty_has(buf, "filtered")) {
        printf("FAIL: filter did not restrict to matching lines\n");
        pty_dump_visible(buf);
        fails++;
    }
    pty_send_text(master, "&"); /* reverse: restore the full document */
    pty_wait_for(master, buf, sizeof buf, "filter cleared", WAIT_MS);
    if (!pty_has(buf, "line001")) {
        printf("FAIL: clearing the filter did not restore the document\n");
        pty_dump_visible(buf);
        fails++;
    }
    pty_send_text(master, "q");
    finish_demo(master, pid, buf, sizeof buf, &status);

    /* Multiple documents: :n / :p switch between files. */
    char doc2_tmpl[4096];
    mk_tmpl(doc2_tmpl, sizeof doc2_tmpl, "paige_doc2_XXXXXX");
    int d2fd = mkstemp(doc2_tmpl);
    if (d2fd < 0) {
        perror("mkstemp");
        unlink(tmpl);
        unlink(follow_tmpl);
        unlink(chop_tmpl);
        return 1;
    }
    for (int i = 1; i <= 20; i++) {
        char line[32];
        int m = snprintf(line, sizeof line, "DOCTWO line %02d\n", i);
        (void)!write(d2fd, line, (size_t)m);
    }
    close(d2fd);
    pid = spawn_demo2(&master, &ws, tmpl, doc2_tmpl);
    if (pid < 0) {
        unlink(tmpl);
        unlink(follow_tmpl);
        unlink(chop_tmpl);
        unlink(doc2_tmpl);
        return 1;
    }
    pty_wait_for(master, buf, sizeof buf, "line001", WAIT_MS); /* first doc */
    pty_send_text(master, ":n");                               /* next file */
    pty_wait_for(master, buf, sizeof buf, "DOCTWO", WAIT_MS);
    if (!pty_has(buf, "DOCTWO") || pty_has(buf, "line001")) {
        printf("FAIL: :n did not switch to the second document\n");
        pty_dump_visible(buf);
        fails++;
    }
    pty_send_text(master, ":p"); /* previous file */
    pty_wait_for(master, buf, sizeof buf, "line001", WAIT_MS);
    if (!pty_has(buf, "line001") || pty_has(buf, "DOCTWO")) {
        printf("FAIL: :p did not switch back to the first document\n");
        pty_dump_visible(buf);
        fails++;
    }
    pty_send_text(master, "q");
    finish_demo(master, pid, buf, sizeof buf, &status);
    unlink(doc2_tmpl);

    unlink(tmpl);
    unlink(follow_tmpl);
    unlink(chop_tmpl);

    if (fails == 0) {
        printf("pty: pager nav/search/goto/quit OK\n");
        return 0;
    }
    return 1;
}
