#include "posix_features.h"
#include "render.h"
#include "term.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *C_RESET = "\x1b[0m";
static const char *C_BOLD  = "\x1b[1m";
static const char *C_DIM   = "\x1b[2m";
static const char *C_GREEN = "\x1b[32m";
static const char *C_YELL  = "\x1b[33m";
static const char *C_RED   = "\x1b[31m";
static const char *C_CYAN  = "\x1b[36m";

/* WORKING: cmd.exe (and the MinGW CRT) treat a console as unbuffered, so
 * each fprintf("...\n") hits the glass immediately. Combined with a
 * leading \x1b[J (erase display) you see: blank flash, then the dashboard
 * painted top-to-bottom, every second. Unix TTYs keep the writes in a
 * stdio buffer until fflush, which is why the Pi and the Mac look still.
 *
 * Fix: build the whole frame in memory and fwrite it once. Live mode
 * homes the cursor and clears each line with \x1b[K instead of wiping
 * the screen first. */
#define FRAME_CAP 24576

typedef struct {
    char   data[FRAME_CAP];
    size_t len;
    int    live; /* 1: \x1b[K before newline so leftover glyphs vanish */
} FrameBuf;

static void fb_add(FrameBuf *fb, const char *s)
{
    size_t n;
    size_t room;

    if (s == NULL || fb->len + 1 >= FRAME_CAP) {
        return;
    }
    n = strlen(s);
    room = FRAME_CAP - 1 - fb->len;
    if (n > room) {
        n = room;
    }
    memcpy(fb->data + fb->len, s, n);
    fb->len += n;
    fb->data[fb->len] = '\0';
}

static void fb_addch(FrameBuf *fb, int c)
{
    if (fb->len + 1 >= FRAME_CAP) {
        return;
    }
    fb->data[fb->len++] = (char)c;
    fb->data[fb->len] = '\0';
}

static void fb_printf(FrameBuf *fb, const char *fmt, ...)
{
    va_list ap;
    int n;
    size_t room = FRAME_CAP - 1 - fb->len;

    if (room == 0) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(fb->data + fb->len, room + 1, fmt, ap);
    va_end(ap);
    if (n > 0) {
        if ((size_t)n > room) {
            fb->len += room;
        } else {
            fb->len += (size_t)n;
        }
    }
}

static void fb_nl(FrameBuf *fb)
{
    /* Erase-to-end-of-line, then newline. Overwrite-in-place, no flash. */
    if (fb->live) {
        fb_add(fb, "\x1b[K");
    }
    fb_addch(fb, '\n');
}

static const char *pct_color(double pct, int color)
{
    if (!color) {
        return "";
    }
    if (pct >= 85.0) {
        return C_RED;
    }
    if (pct >= 60.0) {
        return C_YELL;
    }
    return C_GREEN;
}

static const char *s(const RenderOptions *opt, const char *code)
{
    return opt->color ? code : "";
}

static void bar(FrameBuf *fb, const RenderOptions *opt, double pct, int width)
{
    const char *on;
    const char *off;
    const char *col;
    int filled;
    int i;

    if (width < 4) {
        width = 4;
    }
    pct = util_clamp_pct(pct);
    filled = (int)(pct / 100.0 * (double)width + 0.5);
    if (filled > width) {
        filled = width;
    }
    /* WORKING: Unicode block + light shade read as a meter at a glance.
     * --ascii is for consoles that still mangle UTF-8 (old cmd.exe). */
    on = opt->ascii ? "#" : "\xE2\x96\x88";   /* █ */
    off = opt->ascii ? "." : "\xE2\x96\x91";  /* ░ */
    col = pct_color(pct, opt->color);

    fb_add(fb, col);
    fb_addch(fb, '[');
    for (i = 0; i < filled; i++) {
        fb_add(fb, on);
    }
    for (i = filled; i < width; i++) {
        fb_add(fb, off);
    }
    fb_addch(fb, ']');
    fb_add(fb, s(opt, C_RESET));
}

static double used_pct(uint64_t used, uint64_t total)
{
    if (total == 0) {
        return 0.0;
    }
    return util_clamp_pct(100.0 * (double)used / (double)total);
}

static void hline(FrameBuf *fb, const RenderOptions *opt, int cols)
{
    int i;
    const char *ch = opt->ascii ? "-" : "\xE2\x94\x80"; /* ─ */
    fb_add(fb, s(opt, C_DIM));
    for (i = 0; i < cols; i++) {
        fb_add(fb, ch);
    }
    fb_add(fb, s(opt, C_RESET));
    fb_nl(fb);
}

static int main_bar_width(int cols)
{
    /* Leave room for the label on the left and "  99.9%" on the right. */
    int w = cols - 36;
    return util_clamp_int(w, 16, 48);
}

static int mini_bar_width(int cols)
{
    (void)cols;
    return 8;
}

static void paint(FrameBuf *fb, const ResourceSnapshot *snap, const RenderOptions *opt)
{
    char b1[64], b2[64], b3[64], when[64];
    time_t now;
    struct tm *tm;
    int cols = opt->cols;
    int bw = main_bar_width(cols);
    int mw = mini_bar_width(cols);
    int i;
    double mpct, spct;

    now = time(NULL);
    tm = localtime(&now);
    if (tm != NULL) {
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", tm);
    } else {
        snprintf(when, sizeof(when), "--");
    }

    fb_add(fb, s(opt, C_BOLD));
    fb_add(fb, "  Cross-Platform Resource Monitor");
    fb_add(fb, s(opt, C_RESET));
    if (opt->paused) {
        fb_add(fb, s(opt, C_YELL));
        fb_add(fb, "   PAUSED");
        fb_add(fb, s(opt, C_RESET));
    }
    fb_nl(fb);

    fb_printf(fb, "  %s%s%s  ·  %s %s  ·  %s  ·  %d cores",
              s(opt, C_CYAN),
              snap->hostname[0] ? snap->hostname : "unknown",
              s(opt, C_RESET),
              snap->host_label[0] ? snap->host_label : snap->os_name,
              snap->os_release,
              snap->arch,
              snap->cpu_count);
    fb_nl(fb);
    hline(fb, opt, cols);

    /* --- CPU --- */
    fb_add(fb, "  CPU     ");
    if (!snap->primed || snap->cpu_total < 0.0) {
        fb_add(fb, s(opt, C_DIM));
        fb_add(fb, "(warming up — rates need two samples)");
        fb_add(fb, s(opt, C_RESET));
        fb_nl(fb);
    } else {
        bar(fb, opt, snap->cpu_total, bw);
        fb_printf(fb, "  %5.1f%%", snap->cpu_total);
        fb_nl(fb);
    }

    if (snap->core_count > 0 && snap->primed) {
        int per_line = (cols >= 100) ? 4 : (cols >= 80 ? 3 : 2);
        int col = 0;
        fb_add(fb, "  ");
        for (i = 0; i < snap->core_count; i++) {
            fb_printf(fb, "%2s ", snap->cores[i].name);
            bar(fb, opt, snap->cores[i].usage, mw);
            fb_printf(fb, " %4.0f%%", snap->cores[i].usage);
            col++;
            if (col >= per_line || i + 1 == snap->core_count) {
                fb_nl(fb);
                if (i + 1 < snap->core_count) {
                    fb_add(fb, "  ");
                }
                col = 0;
            } else {
                fb_add(fb, "   ");
            }
        }
    }
    fb_nl(fb);

    /* --- Memory --- */
    mpct = used_pct(snap->mem_used, snap->mem_total);
    util_format_bytes(snap->mem_used, b1, sizeof(b1));
    util_format_bytes(snap->mem_total, b2, sizeof(b2));
    fb_add(fb, "  Memory  ");
    bar(fb, opt, mpct, bw);
    fb_printf(fb, "  %s / %s  (%4.1f%%)", b1, b2, mpct);
    fb_nl(fb);

    if (snap->swap_total > 0) {
        spct = used_pct(snap->swap_used, snap->swap_total);
        util_format_bytes(snap->swap_used, b1, sizeof(b1));
        util_format_bytes(snap->swap_total, b2, sizeof(b2));
        fb_add(fb, "  Swap    ");
        bar(fb, opt, spct, bw);
        fb_printf(fb, "  %s / %s  (%4.1f%%)", b1, b2, spct);
        fb_nl(fb);
    } else {
        fb_add(fb, s(opt, C_DIM));
        fb_add(fb, "  Swap    none");
        fb_add(fb, s(opt, C_RESET));
        fb_nl(fb);
    }
    fb_nl(fb);

    /* --- Disks --- */
    if (snap->disk_count == 0) {
        fb_add(fb, s(opt, C_DIM));
        fb_add(fb, "  Disk    (none reported)");
        fb_add(fb, s(opt, C_RESET));
        fb_nl(fb);
    }
    for (i = 0; i < snap->disk_count; i++) {
        double dp = used_pct(snap->disks[i].used_bytes, snap->disks[i].total_bytes);
        char mount[28];
        snprintf(mount, sizeof(mount), "%s", snap->disks[i].mount);
        util_format_bytes(snap->disks[i].used_bytes, b1, sizeof(b1));
        util_format_bytes(snap->disks[i].total_bytes, b2, sizeof(b2));
        fb_printf(fb, "  Disk %-18s ", mount);
        bar(fb, opt, dp, util_clamp_int(bw - 10, 10, 40));
        fb_printf(fb, "  %s / %s  %s%s%s",
                  b1, b2, s(opt, C_DIM), snap->disks[i].fstype, s(opt, C_RESET));
        fb_nl(fb);
    }
    fb_nl(fb);

    /* --- Network --- */
    {
        int shown = 0;
        int any_bytes = 0;
        for (i = 0; i < snap->net_count; i++) {
            if (snap->nets[i].rx_bytes + snap->nets[i].tx_bytes > 0) {
                any_bytes = 1;
                break;
            }
        }
        for (i = 0; i < snap->net_count; i++) {
            /* WORKING: Thunderbolt / unused Ethernet ports show up as
             * enN with zero lifetime counters. Hide those unless every
             * NIC is idle (a fresh VM should still list eth0). */
            if (any_bytes &&
                snap->nets[i].rx_bytes == 0 && snap->nets[i].tx_bytes == 0) {
                continue;
            }
            util_format_rate(snap->nets[i].rx_bps, b1, sizeof(b1));
            util_format_rate(snap->nets[i].tx_bps, b2, sizeof(b2));
            fb_printf(fb, "  Net  %-8s   %s↓%s %-10s    %s↑%s %s",
                      snap->nets[i].name,
                      s(opt, C_CYAN), s(opt, C_RESET), b1,
                      s(opt, C_YELL), s(opt, C_RESET), b2);
            fb_nl(fb);
            shown++;
        }
        if (shown == 0) {
            fb_add(fb, s(opt, C_DIM));
            fb_add(fb, "  Net     (no active interfaces)");
            fb_add(fb, s(opt, C_RESET));
            fb_nl(fb);
        }
    }
    fb_nl(fb);

    /* --- Footer facts --- */
    util_format_uptime(snap->uptime_sec, b1, sizeof(b1));
    if (snap->load_valid) {
        fb_printf(fb, "  Load  %5.2f  %5.2f  %5.2f     Processes  %-5u     Up  %s",
                  snap->load1, snap->load5, snap->load15,
                  (unsigned)snap->process_count, b1);
        fb_nl(fb);
    } else {
        fb_printf(fb, "  Processes  %-5u     Up  %s",
                  (unsigned)snap->process_count, b1);
        fb_nl(fb);
    }

    if (snap->battery_present) {
        double bp = snap->battery_percent >= 0 ? (double)snap->battery_percent : 0.0;
        fb_add(fb, "  Battery ");
        if (snap->battery_percent >= 0) {
            bar(fb, opt, bp, 12);
            fb_printf(fb, "  %3d%%", snap->battery_percent);
        } else {
            fb_add(fb, s(opt, C_DIM));
            fb_add(fb, "  unknown %");
            fb_add(fb, s(opt, C_RESET));
        }
        if (snap->battery_charging) {
            fb_add(fb, "  charging");
        } else if (snap->battery_plugged) {
            fb_add(fb, "  on AC");
        } else {
            fb_add(fb, "  on battery");
        }
        fb_nl(fb);
    }

    hline(fb, opt, cols);
    snprintf(b3, sizeof(b3), "%.1fs", opt->interval_sec);
    fb_printf(fb, "  %sq%s quit   %sspace%s pause   refresh %s          %s",
              s(opt, C_BOLD), s(opt, C_RESET),
              s(opt, C_BOLD), s(opt, C_RESET),
              b3, when);
    fb_nl(fb);
}

static void fb_flush(const FrameBuf *fb)
{
    if (fb->len > 0) {
        fwrite(fb->data, 1, fb->len, stdout);
        fflush(stdout);
    }
}

void render_dashboard(const ResourceSnapshot *snap, const RenderOptions *opt)
{
    FrameBuf fb;

    memset(&fb, 0, sizeof(fb));
    fb.live = 1;
    /* Home only — do not erase the display first (that is the cmd flash). */
    fb_add(&fb, "\x1b[H");
    paint(&fb, snap, opt);
    fb_add(&fb, "\x1b[J");
    fb_flush(&fb);
}

void render_once(const ResourceSnapshot *snap, const RenderOptions *opt)
{
    FrameBuf fb;

    memset(&fb, 0, sizeof(fb));
    fb.live = 0;
    paint(&fb, snap, opt);
    fb_flush(&fb);
}
