#include "posix_features.h"
#include "render.h"
#include "term.h"
#include "util.h"

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

static void bar(FILE *out, const RenderOptions *opt, double pct, int width)
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

    fputs(col, out);
    fputc('[', out);
    for (i = 0; i < filled; i++) {
        fputs(on, out);
    }
    for (i = filled; i < width; i++) {
        fputs(off, out);
    }
    fputc(']', out);
    fputs(s(opt, C_RESET), out);
}

static double used_pct(uint64_t used, uint64_t total)
{
    if (total == 0) {
        return 0.0;
    }
    return util_clamp_pct(100.0 * (double)used / (double)total);
}

static void hline(FILE *out, const RenderOptions *opt, int cols)
{
    int i;
    const char *ch = opt->ascii ? "-" : "\xE2\x94\x80"; /* ─ */
    fputs(s(opt, C_DIM), out);
    for (i = 0; i < cols; i++) {
        fputs(ch, out);
    }
    fputs(s(opt, C_RESET), out);
    fputc('\n', out);
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

static void paint(FILE *out, const ResourceSnapshot *snap, const RenderOptions *opt)
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

    fputs(s(opt, C_BOLD), out);
    fputs("  Cross-Platform Resource Monitor", out);
    fputs(s(opt, C_RESET), out);
    if (opt->paused) {
        fputs(s(opt, C_YELL), out);
        fputs("   PAUSED", out);
        fputs(s(opt, C_RESET), out);
    }
    fputc('\n', out);

    fprintf(out, "  %s%s%s  ·  %s %s  ·  %s  ·  %d cores\n",
            s(opt, C_CYAN),
            snap->hostname[0] ? snap->hostname : "unknown",
            s(opt, C_RESET),
            snap->host_label[0] ? snap->host_label : snap->os_name,
            snap->os_release,
            snap->arch,
            snap->cpu_count);
    hline(out, opt, cols);

    /* --- CPU --- */
    fputs("  CPU     ", out);
    if (!snap->primed || snap->cpu_total < 0.0) {
        fputs(s(opt, C_DIM), out);
        fputs("(warming up — rates need two samples)\n", out);
        fputs(s(opt, C_RESET), out);
    } else {
        bar(out, opt, snap->cpu_total, bw);
        fprintf(out, "  %5.1f%%\n", snap->cpu_total);
    }

    if (snap->core_count > 0 && snap->primed) {
        int per_line = (cols >= 100) ? 4 : (cols >= 80 ? 3 : 2);
        int col = 0;
        fputs("  ", out);
        for (i = 0; i < snap->core_count; i++) {
            fprintf(out, "%2s ", snap->cores[i].name);
            bar(out, opt, snap->cores[i].usage, mw);
            fprintf(out, " %4.0f%%", snap->cores[i].usage);
            col++;
            if (col >= per_line || i + 1 == snap->core_count) {
                fputc('\n', out);
                if (i + 1 < snap->core_count) {
                    fputs("  ", out);
                }
                col = 0;
            } else {
                fputs("   ", out);
            }
        }
    }
    fputc('\n', out);

    /* --- Memory --- */
    mpct = used_pct(snap->mem_used, snap->mem_total);
    util_format_bytes(snap->mem_used, b1, sizeof(b1));
    util_format_bytes(snap->mem_total, b2, sizeof(b2));
    fputs("  Memory  ", out);
    bar(out, opt, mpct, bw);
    fprintf(out, "  %s / %s  (%4.1f%%)\n", b1, b2, mpct);

    if (snap->swap_total > 0) {
        spct = used_pct(snap->swap_used, snap->swap_total);
        util_format_bytes(snap->swap_used, b1, sizeof(b1));
        util_format_bytes(snap->swap_total, b2, sizeof(b2));
        fputs("  Swap    ", out);
        bar(out, opt, spct, bw);
        fprintf(out, "  %s / %s  (%4.1f%%)\n", b1, b2, spct);
    } else {
        fputs(s(opt, C_DIM), out);
        fputs("  Swap    none\n", out);
        fputs(s(opt, C_RESET), out);
    }
    fputc('\n', out);

    /* --- Disks --- */
    if (snap->disk_count == 0) {
        fputs(s(opt, C_DIM), out);
        fputs("  Disk    (none reported)\n", out);
        fputs(s(opt, C_RESET), out);
    }
    for (i = 0; i < snap->disk_count; i++) {
        double dp = used_pct(snap->disks[i].used_bytes, snap->disks[i].total_bytes);
        char mount[28];
        snprintf(mount, sizeof(mount), "%s", snap->disks[i].mount);
        util_format_bytes(snap->disks[i].used_bytes, b1, sizeof(b1));
        util_format_bytes(snap->disks[i].total_bytes, b2, sizeof(b2));
        fprintf(out, "  Disk %-18s ", mount);
        bar(out, opt, dp, util_clamp_int(bw - 10, 10, 40));
        fprintf(out, "  %s / %s  %s%s%s\n",
                b1, b2, s(opt, C_DIM), snap->disks[i].fstype, s(opt, C_RESET));
    }
    fputc('\n', out);

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
            fprintf(out, "  Net  %-8s   %s↓%s %-10s    %s↑%s %s\n",
                    snap->nets[i].name,
                    s(opt, C_CYAN), s(opt, C_RESET), b1,
                    s(opt, C_YELL), s(opt, C_RESET), b2);
            shown++;
        }
        if (shown == 0) {
            fputs(s(opt, C_DIM), out);
            fputs("  Net     (no active interfaces)\n", out);
            fputs(s(opt, C_RESET), out);
        }
    }
    fputc('\n', out);

    /* --- Footer facts --- */
    util_format_uptime(snap->uptime_sec, b1, sizeof(b1));
    if (snap->load_valid) {
        fprintf(out, "  Load  %5.2f  %5.2f  %5.2f     Processes  %-5u     Up  %s\n",
                snap->load1, snap->load5, snap->load15,
                (unsigned)snap->process_count, b1);
    } else {
        fprintf(out, "  Processes  %-5u     Up  %s\n",
                (unsigned)snap->process_count, b1);
    }

    if (snap->battery_present) {
        double bp = snap->battery_percent >= 0 ? (double)snap->battery_percent : 0.0;
        fputs("  Battery ", out);
        if (snap->battery_percent >= 0) {
            bar(out, opt, bp, 12);
            fprintf(out, "  %3d%%", snap->battery_percent);
        } else {
            fputs(s(opt, C_DIM), out);
            fputs("  unknown %", out);
            fputs(s(opt, C_RESET), out);
        }
        if (snap->battery_charging) {
            fputs("  charging", out);
        } else if (snap->battery_plugged) {
            fputs("  on AC", out);
        } else {
            fputs("  on battery", out);
        }
        fputc('\n', out);
    }

    hline(out, opt, cols);
    snprintf(b3, sizeof(b3), "%.1fs", opt->interval_sec);
    fprintf(out, "  %sq%s quit   %sspace%s pause   refresh %s          %s\n",
            s(opt, C_BOLD), s(opt, C_RESET),
            s(opt, C_BOLD), s(opt, C_RESET),
            b3, when);
}

void render_dashboard(const ResourceSnapshot *snap, const RenderOptions *opt)
{
    term_begin_frame();
    paint(stdout, snap, opt);
    fflush(stdout);
}

void render_once(const ResourceSnapshot *snap, const RenderOptions *opt)
{
    paint(stdout, snap, opt);
    fflush(stdout);
}
