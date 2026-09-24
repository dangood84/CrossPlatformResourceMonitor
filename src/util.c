#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

void util_sleep_ms(int ms)
{
    if (ms < 0) {
        ms = 0;
    }
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    /* WORKING: nanosleep is restartable; a SIGWINCH mid-sleep should not
     * skip the rest of the interval or the dashboard stutters. */
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&req, &req) != 0) {
        /* EINTR: req now holds the remainder. Anything else: give up. */
        if (req.tv_sec <= 0 && req.tv_nsec <= 0) {
            break;
        }
    }
#endif
}

double util_now_sec(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int got_freq = 0;
    LARGE_INTEGER now;
    if (!got_freq) {
        QueryPerformanceFrequency(&freq);
        got_freq = 1;
    }
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

int util_clamp_int(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

double util_clamp_pct(double v)
{
    if (v < 0.0) {
        return 0.0;
    }
    if (v > 100.0) {
        return 100.0;
    }
    return v;
}

static void format_scaled(double value, const char *const units[], int nunits,
                          char *buf, size_t n)
{
    int u = 0;
    while (u < nunits - 1 && value >= 1024.0) {
        value /= 1024.0;
        u++;
    }
    if (u == 0) {
        snprintf(buf, n, "%.0f %s", value, units[u]);
    } else if (value >= 10.0) {
        snprintf(buf, n, "%.1f %s", value, units[u]);
    } else {
        snprintf(buf, n, "%.2f %s", value, units[u]);
    }
}

void util_format_bytes(uint64_t bytes, char *buf, size_t n)
{
    static const char *units[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    format_scaled((double)bytes, units, 6, buf, n);
}

void util_format_rate(double bps, char *buf, size_t n)
{
    static const char *units[] = { "B/s", "KB/s", "MB/s", "GB/s", "TB/s" };
    if (bps < 0.0) {
        bps = 0.0;
    }
    format_scaled(bps, units, 5, buf, n);
}

void util_format_uptime(uint64_t seconds, char *buf, size_t n)
{
    uint64_t days = seconds / 86400;
    uint64_t hours = (seconds % 86400) / 3600;
    uint64_t mins = (seconds % 3600) / 60;
    if (days > 0) {
        snprintf(buf, n, "%llud %lluh %llum",
                 (unsigned long long)days,
                 (unsigned long long)hours,
                 (unsigned long long)mins);
    } else if (hours > 0) {
        snprintf(buf, n, "%lluh %llum",
                 (unsigned long long)hours,
                 (unsigned long long)mins);
    } else {
        snprintf(buf, n, "%llum %llus",
                 (unsigned long long)mins,
                 (unsigned long long)(seconds % 60));
    }
}

int util_nocolor_requested(void)
{
    /* https://no-color.org/ — any non-empty value means "please don't". */
    const char *e = getenv("NO_COLOR");
    return e != NULL && e[0] != '\0';
}
