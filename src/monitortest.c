#include "posix_features.h"
#include "collect.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

/* Headless checks — the same role as calctest.pas in Goody's Calculator.
 * No raw terminal, no dashboard. Two samples so CPU rates are primed. */

static int fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

int main(void)
{
    ResourceSnapshot a;
    ResourceSnapshot b;
    int rc = 0;

    if (collect_init() != 0) {
        return fail("collect_init");
    }

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    if (collect_snapshot(&a) != 0) {
        collect_shutdown();
        return fail("first collect_snapshot");
    }
    if (a.hostname[0] == '\0') {
        rc |= fail("hostname empty");
    }
    if (a.cpu_count <= 0) {
        rc |= fail("cpu_count <= 0");
    }
    if (a.mem_total == 0) {
        rc |= fail("mem_total is 0");
    }
    if (a.primed) {
        rc |= fail("first snapshot should not be primed");
    }

    util_sleep_ms(250);
    if (collect_snapshot(&b) != 0) {
        collect_shutdown();
        return fail("second collect_snapshot");
    }
    if (!b.primed) {
        rc |= fail("second snapshot should be primed");
    }
    if (b.cpu_total < 0.0 || b.cpu_total > 100.0) {
        rc |= fail("cpu_total out of range");
    }
    if (b.mem_used > b.mem_total) {
        rc |= fail("mem_used > mem_total");
    }
    if (b.sample_dt <= 0.0) {
        rc |= fail("sample_dt should be > 0 after two reads");
    }

    printf("ok  host=%s  hostname=%s  cpus=%d  cpu=%.1f%%  mem=%llu/%llu  "
           "disks=%d  nets=%d  procs=%u  up=%llu  primed=%d  dt=%.3fs\n",
           collect_host_name(),
           b.hostname,
           b.cpu_count,
           b.cpu_total,
           (unsigned long long)b.mem_used,
           (unsigned long long)b.mem_total,
           b.disk_count,
           b.net_count,
           (unsigned)b.process_count,
           (unsigned long long)b.uptime_sec,
           b.primed,
           b.sample_dt);

    collect_shutdown();
    return rc;
}
