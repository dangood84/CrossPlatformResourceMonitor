#include "posix_features.h"
#include "collect.h"
#include "render.h"
#include "term.h"
#include "util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif
#ifdef __linux__
#include <limits.h>
#endif

/* WORKING: SIGINT/SIGTERM must not leave the terminal in raw mode or
 * the user's shell looks broken after Ctrl+C. atexit in term_init also
 * restores; the flag lets the loop exit cleanly instead of _Exit. */
static volatile sig_atomic_t g_stop = 0;

static void on_stop(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(FILE *out)
{
    fprintf(out,
        "Cross-Platform Resource Monitor\n"
        "\n"
        "Usage: resource-monitor [options]\n"
        "\n"
        "  -1, --once          print one snapshot and exit (no raw terminal)\n"
        "  -n, --interval SEC  refresh interval (default 1.0, min 0.2, max 60)\n"
        "      --ascii         '#' / '.' bars instead of Unicode blocks\n"
        "      --no-color      disable ANSI colours (also honours NO_COLOR)\n"
        "  -h, --help          this text\n"
        "\n"
        "Keys (live mode):  q quit   space pause/resume\n"
        "\n"
        "See README.md, WORKINGS.md, and EXECUTION_FLOW.md.\n");
}

#ifdef __linux__
/* WORKING: `open` is a macOS command. On the Pi, xdg-open / a file-manager
 * double-click starts this binary with no controlling terminal. The live
 * dashboard needs a TTY, so without help it primed, printed one frame to
 * nowhere, and exited — "nothing happens".
 *
 * If stdin/stdout/stderr are all non-TTYs and the user did not pass
 * --once, open a real terminal emulator and re-exec ourselves. The env
 * var stops a fork bomb if the child is still detached. */
static int linux_spawn_in_terminal(const char *argv0)
{
    char self[PATH_MAX];
    const char *path = argv0;
    ssize_t n;
    pid_t pid;

    if (getenv("RESOURCE_MONITOR_IN_TERM") != NULL) {
        return -1;
    }

    n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        path = self;
    }
    if (path == NULL || path[0] == '\0') {
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        setenv("RESOURCE_MONITOR_IN_TERM", "1", 1);
        execlp("x-terminal-emulator", "x-terminal-emulator", "-e", path, (char *)NULL);
        execlp("lxterminal", "lxterminal", "-e", path, (char *)NULL);
        execlp("xterm", "xterm", "-e", path, (char *)NULL);
        _exit(127);
    }
    return 0;
}
#endif

static double parse_interval(const char *s)
{
    double v = atof(s);
    if (v < 0.2) {
        v = 0.2;
    }
    if (v > 60.0) {
        v = 60.0;
    }
    return v;
}

int main(int argc, char **argv)
{
    int once = 0;
    int ascii = 0;
    int color = 1;
    double interval = 1.0;
    int i;
    ResourceSnapshot snap;
    RenderOptions opt;
    int paused = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-1") == 0 || strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else if (strcmp(argv[i], "--ascii") == 0) {
            ascii = 1;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            color = 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else if ((strcmp(argv[i], "-n") == 0 ||
                    strcmp(argv[i], "--interval") == 0) && i + 1 < argc) {
            interval = parse_interval(argv[++i]);
        } else if (strncmp(argv[i], "--interval=", 11) == 0) {
            interval = parse_interval(argv[i] + 11);
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }

    if (util_nocolor_requested()) {
        color = 0;
    }

#ifdef __linux__
    if (!once &&
        !isatty(STDIN_FILENO) &&
        !isatty(STDOUT_FILENO) &&
        !isatty(STDERR_FILENO)) {
        if (linux_spawn_in_terminal(argc > 0 ? argv[0] : NULL) == 0) {
            return 0;
        }
        fprintf(stderr,
                "resource-monitor: this is a terminal dashboard.\n"
                "Run it from a terminal, not with 'open' or a silent GUI click:\n"
                "  ./build/resource-monitor\n"
                "  ./build/resource-monitor --once\n");
        return 1;
    }
#endif

    if (collect_init() != 0) {
        fprintf(stderr, "collect_init failed on %s\n", collect_host_name());
        return 1;
    }

#ifdef _WIN32
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
#else
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_stop;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        /* WORKING: SIGPIPE is irrelevant (we only write stdout) but
         * SIGWINCH is ignored: the next frame re-reads TIOCGWINSZ. */
        signal(SIGPIPE, SIG_IGN);
    }
#endif

    memset(&snap, 0, sizeof(snap));

    /* Prime: first snapshot stores counters, second computes rates. */
    if (collect_snapshot(&snap) != 0) {
        fprintf(stderr, "collect_snapshot failed\n");
        collect_shutdown();
        return 1;
    }
    util_sleep_ms((int)(interval * 1000.0));
    if (collect_snapshot(&snap) != 0) {
        fprintf(stderr, "collect_snapshot failed\n");
        collect_shutdown();
        return 1;
    }

    memset(&opt, 0, sizeof(opt));
    opt.ascii = ascii;
    opt.color = color;
    opt.interval_sec = interval;
    opt.cols = 80;

    if (once) {
        render_once(&snap, &opt);
        collect_shutdown();
        return 0;
    }

    if (!term_is_tty()) {
        /* WORKING: piped to a file or `less`, a live alt-screen loop is
         * the wrong tool. Print one framed snapshot the way --once does.
         * A fully detached GUI launch is handled earlier on Linux. */
        if (!once) {
            fprintf(stderr,
                    "resource-monitor: no TTY on stdout; printing one snapshot.\n"
                    "For the live dashboard run this in a terminal:\n"
                    "  ./build/resource-monitor\n");
        }
        render_once(&snap, &opt);
        collect_shutdown();
        return 0;
    }

    term_init();

    while (!g_stop) {
        int key;
        int cols = 80, rows = 24;
        (void)rows;
        term_size(&cols, &rows);
        opt.cols = cols;
        opt.paused = paused;

        if (!paused) {
            if (collect_snapshot(&snap) != 0) {
                break;
            }
        }
        render_dashboard(&snap, &opt);

        key = term_poll_key((int)(interval * 1000.0));
        if (g_stop) {
            break;
        }
        if (key == 'q' || key == 'Q' || key == 3) {
            break;
        }
        if (key == ' ') {
            paused = !paused;
        }
    }

    term_restore();
    collect_shutdown();
    return 0;
}
