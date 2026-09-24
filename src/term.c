#include "posix_features.h"
#include "term.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <conio.h>
#include <io.h>
#else
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

static int g_inited = 0;

#ifdef _WIN32
static HANDLE g_out = INVALID_HANDLE_VALUE;
static HANDLE g_in = INVALID_HANDLE_VALUE;
static DWORD g_out_mode = 0;
static DWORD g_in_mode = 0;
static int g_got_out_mode = 0;
static int g_got_in_mode = 0;
static UINT g_old_cp = 0;
#else
static struct termios g_old;
static int g_got_old = 0;
#endif

static void restore_atexit(void)
{
    term_restore();
}

int term_is_tty(void)
{
#ifdef _WIN32
    return _isatty(_fileno(stdout));
#else
    return isatty(STDOUT_FILENO);
#endif
}

int term_init(void)
{
    if (g_inited) {
        return 0;
    }

#ifdef _WIN32
    /* WORKING: Windows 10+ can speak the same ANSI sequences as a Unix
     * terminal once ENABLE_VIRTUAL_TERMINAL_PROCESSING is on. Without it
     * the dashboard is a mess of escape codes. We also force UTF-8 so the
     * block-character bars are not "??". */
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    if (g_out != INVALID_HANDLE_VALUE && GetConsoleMode(g_out, &g_out_mode)) {
        DWORD mode = g_out_mode;
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT;
        SetConsoleMode(g_out, mode);
        g_got_out_mode = 1;
    }
    if (g_in != INVALID_HANDLE_VALUE && GetConsoleMode(g_in, &g_in_mode)) {
        DWORD mode = g_in_mode;
        mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
        mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
        SetConsoleMode(g_in, mode);
        g_got_in_mode = 1;
    }
    g_old_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
#else
    if (tcgetattr(STDIN_FILENO, &g_old) == 0) {
        struct termios raw = g_old;
        /* WORKING: ICANON+ECHO off so a single 'q' is readable without
         * Enter. ISIG stays on so Ctrl+C still raises SIGINT and the
         * signal handler can restore the terminal. */
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        g_got_old = 1;
    }
#endif

    /* Alt screen + hide cursor. The previous scrollback comes back on
     * restore, so a long run does not leave 400 dashboard frames behind. */
    fputs("\x1b[?1049h\x1b[?25l", stdout);
    fflush(stdout);

    atexit(restore_atexit);
    g_inited = 1;
    return 0;
}

void term_restore(void)
{
    if (!g_inited) {
        return;
    }
    fputs("\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);

#ifdef _WIN32
    if (g_got_out_mode && g_out != INVALID_HANDLE_VALUE) {
        SetConsoleMode(g_out, g_out_mode);
    }
    if (g_got_in_mode && g_in != INVALID_HANDLE_VALUE) {
        SetConsoleMode(g_in, g_in_mode);
    }
    if (g_old_cp != 0) {
        SetConsoleOutputCP(g_old_cp);
    }
#else
    if (g_got_old) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_old);
    }
#endif
    g_inited = 0;
}

int term_poll_key(int timeout_ms)
{
#ifdef _WIN32
    DWORD start = GetTickCount();
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    while ((DWORD)(GetTickCount() - start) < (DWORD)timeout_ms) {
        if (_kbhit()) {
            int c = _getch();
            /* WORKING: function/arrow keys arrive as 0 or 224 plus a
             * follow-up. Swallow the pair so an arrow does not look
             * like a command. */
            if (c == 0 || c == 224) {
                (void)_getch();
                continue;
            }
            return c;
        }
        Sleep(20);
    }
    return 0;
#else
    struct pollfd p;
    unsigned char c;

    p.fd = STDIN_FILENO;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, timeout_ms) <= 0) {
        return 0;
    }
    if (read(STDIN_FILENO, &c, 1) != 1) {
        return 0;
    }
    if (c == 0x1b) {
        /* Drain a CSI sequence (arrows, etc.) so leftover bytes do not
         * become a mystery key on the next poll. */
        struct pollfd extra;
        extra.fd = STDIN_FILENO;
        extra.events = POLLIN;
        extra.revents = 0;
        while (poll(&extra, 1, 0) > 0) {
            unsigned char dump;
            if (read(STDIN_FILENO, &dump, 1) != 1) {
                break;
            }
        }
        return 0;
    }
    return (int)c;
#endif
}

void term_size(int *cols, int *rows)
{
    int c = 80;
    int r = 24;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (g_out != INVALID_HANDLE_VALUE &&
        GetConsoleScreenBufferInfo(g_out, &info)) {
        c = info.srWindow.Right - info.srWindow.Left + 1;
        r = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col > 0 && ws.ws_row > 0) {
        c = ws.ws_col;
        r = ws.ws_row;
    }
#endif
    if (cols) {
        *cols = util_clamp_int(c, 60, 240);
    }
    if (rows) {
        *rows = util_clamp_int(r, 16, 80);
    }
}

void term_begin_frame(void)
{
    /* Home + clear-down. Cheaper than a full 2J and avoids flicker. */
    fputs("\x1b[H\x1b[J", stdout);
}
