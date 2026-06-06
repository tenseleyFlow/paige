/* pty_driver: run a pager in a pty and time visible terminal updates. */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <termios.h>
#if defined(__linux__)
#include <pty.h>
#elif defined(__APPLE__)
#include <util.h>
#else
#include <libutil.h>
#endif

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static double now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static void usage(void)
{
    fprintf(stderr, "usage:\n"
                    "  pty_driver first NAME -- COMMAND [ARG...]\n"
                    "  pty_driver jump NAME -- COMMAND [ARG...]\n"
                    "  pty_driver search NAME NEEDLE -- COMMAND [ARG...]\n");
}

static int find_sep(int argc, char **argv, int start)
{
    for (int i = start; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0)
            return i;
    }
    return -1;
}

static void write_key(int fd, const char *s)
{
    (void)!write(fd, s, strlen(s));
}

/* Pull "key=N" out of the demo's paige-stats line (or -1 if absent, e.g. for
 * less/ov/moar). These counters isolate the engine's work from the host's
 * file slurp+index, which the wall-clock first_ms below cannot. */
static long parse_field(const char *s, const char *key)
{
    const char *p = strstr(s, key);
    return p ? atol(p + strlen(key)) : -1;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        usage();
        return 2;
    }
    const char *mode = argv[1];
    const char *name = argv[2];
    const char *needle = NULL;
    int sep_start = 3;
    if (strcmp(mode, "search") == 0) {
        if (argc < 6) {
            usage();
            return 2;
        }
        needle = argv[3];
        sep_start = 4;
    } else if (strcmp(mode, "first") != 0 && strcmp(mode, "jump") != 0) {
        usage();
        return 2;
    }

    int sep = find_sep(argc, argv, sep_start);
    if (sep < 0 || sep + 1 >= argc) {
        usage();
        return 2;
    }

    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = 24;
    ws.ws_col = 80;

    int master;
    double start = now_ms();
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) {
        perror("forkpty");
        return 1;
    }
    if (pid == 0) {
        execvp(argv[sep + 1], &argv[sep + 1]);
        _exit(127);
    }

    enum { TIMEOUT_MS = 15000, IDLE_MS = 120, SETTLE_MS = 120 };
    char buf[8192];
    size_t pty_bytes = 0, pty_reads = 0;
    double first_ms = -1.0, elapsed_ms = -1.0;
    double action_start = 0.0, last_read = 0.0;
    int action_sent = 0, action_output = 0, done = 0;
    const char *status = "ok";

    for (;;) {
        double now = now_ms();
        if (now - start > (double)TIMEOUT_MS) {
            status = "timeout";
            break;
        }
        if (action_sent && action_output &&
            now - last_read >= (double)IDLE_MS) {
            elapsed_ms = last_read - action_start;
            done = 1;
            break;
        }
        /* Send the action only once the INITIAL frame has settled. Sending it
         * on the first byte (as we used to) let the initial frame's tail be
         * mistaken for the response, so a host that then stalls -- e.g. an
         * O(file) jump-to-end that goes silent for a second before redrawing --
         * was concluded during the stall and reported ~0ms. We now wait out the
         * initial frame, then time from the keypress to the response settling.
         */
        if (!action_sent && first_ms >= 0.0 && strcmp(mode, "first") != 0 &&
            now - last_read >= (double)SETTLE_MS) {
            if (strcmp(mode, "jump") == 0) {
                action_start = now_ms();
                write_key(master, "G");
            } else {
                char query[512];
                snprintf(query, sizeof query, "/%s\n", needle ? needle : "");
                action_start = now_ms();
                write_key(master, query);
            }
            action_sent = 1;
        }

        struct pollfd pfd = {master, POLLIN, 0};
        int pr = poll(&pfd, 1, 25);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            status = "poll-error";
            break;
        }
        if (pr == 0)
            continue;
        ssize_t n = read(master, buf, sizeof buf);
        if (n <= 0)
            break;
        now = now_ms();
        pty_bytes += (size_t)n;
        pty_reads++;
        last_read = now;

        if (first_ms < 0.0) {
            first_ms = now - start;
            if (strcmp(mode, "first") == 0) {
                elapsed_ms = first_ms;
                done = 1;
                break;
            }
            continue; /* action is sent later, once the initial frame settles */
        }
        if (action_sent)
            action_output = 1;
    }

    char tail[8192];
    size_t tail_len = 0;
    write_key(master, "q");
    double drain_start = now_ms();
    while (now_ms() - drain_start < 1000.0) {
        struct pollfd pfd = {master, POLLIN, 0};
        if (poll(&pfd, 1, 25) <= 0)
            break;
        ssize_t n = read(master, buf, sizeof buf);
        if (n <= 0)
            break;
        pty_bytes += (size_t)n;
        pty_reads++;
        /* Keep the tail of the drained output so we can read the paige-stats
         * line the demo prints on quit (with PAIGE_STATS set). */
        size_t add = (size_t)n < sizeof tail - 1 ? (size_t)n : sizeof tail - 1;
        if (tail_len + add > sizeof tail - 1) {
            size_t drop = tail_len + add - (sizeof tail - 1);
            memmove(tail, tail + drop, tail_len - drop);
            tail_len -= drop;
        }
        memcpy(tail + tail_len, buf + ((size_t)n - add), add);
        tail_len += add;
    }
    tail[tail_len] = '\0';

    struct rusage ru;
    memset(&ru, 0, sizeof ru);
    int child_status = 0;
    pid_t waited;
    int waited_ms = 0;
    while ((waited = wait4(pid, &child_status, WNOHANG, &ru)) == 0 &&
           waited_ms < 1000) {
        usleep(10000);
        waited_ms += 10;
    }
    if (waited == 0) {
        kill(pid, SIGTERM);
        (void)wait4(pid, &child_status, 0, &ru);
    }
    close(master);

    if (!done && strcmp(status, "ok") == 0)
        status = "no-output";
    printf(
        "bench mode=%s name=%s elapsed_ms=%.3f first_ms=%.3f "
        "rss_max=%ld pty_bytes=%zu pty_reads=%zu render=%ld search_lines=%ld "
        "segments=%ld status=%s\n",
        mode, name, elapsed_ms, first_ms, ru.ru_maxrss, pty_bytes, pty_reads,
        parse_field(tail, "render="), parse_field(tail, "search_lines="),
        parse_field(tail, "segments="), status);
    return strcmp(status, "ok") == 0 ? 0 : 1;
}
