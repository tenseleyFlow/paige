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

    pty_send_text(master, "/line05\n"); /* forward search */
    pty_wait_for(master, buf, sizeof buf, "line 50", WAIT_MS);
    if (!pty_has(buf, "line 50")) {
        printf("FAIL: search for line05 did not land on line 50\n");
        pty_dump_visible(buf);
        fails++;
    }
    if (!pty_has(buf, "\x1b[7mline05")) {
        printf("FAIL: search highlight missing\n");
        fails++;
    }

    pty_send_text(master, "n"); /* next match */
    pty_wait_for(master, buf, sizeof buf, "line 51", WAIT_MS);
    if (!pty_has(buf, "line 51")) {
        printf("FAIL: 'n' did not advance to next search match\n");
        pty_dump_visible(buf);
        fails++;
    }

    pty_send_text(master, "N"); /* previous match */
    pty_wait_for(master, buf, sizeof buf, "line 50", WAIT_MS);
    if (!pty_has(buf, "line 50")) {
        printf("FAIL: 'N' did not return to previous search match\n");
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
    unlink(tmpl);
    unlink(follow_tmpl);
    unlink(chop_tmpl);

    if (fails == 0) {
        printf("pty: pager nav/search/goto/quit OK\n");
        return 0;
    }
    return 1;
}
