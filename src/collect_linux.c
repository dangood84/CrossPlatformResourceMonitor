#include "collect.h"
#include "util.h"

#include <dirent.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

typedef struct {
    uint64_t idle;
    uint64_t total;
} CpuTicks;

static CpuTicks g_cpu_all;
static CpuTicks g_cpu_core[SNAP_MAX_CORES];
static uint64_t g_net_rx[SNAP_MAX_NETS];
static uint64_t g_net_tx[SNAP_MAX_NETS];
static char     g_net_name[SNAP_MAX_NETS][SNAP_NAME_LEN];
static int      g_net_n;
static int      g_primed;
static double   g_last_t;

const char *collect_host_name(void)
{
    return "Linux";
}

int collect_init(void)
{
    memset(&g_cpu_all, 0, sizeof(g_cpu_all));
    memset(g_cpu_core, 0, sizeof(g_cpu_core));
    g_net_n = 0;
    g_primed = 0;
    g_last_t = 0.0;
    return 0;
}

void collect_shutdown(void)
{
}

static int skip_iface(const char *name)
{
    static const char *pref[] = {
        "lo", "docker", "br-", "veth", "virbr", "tun", "tap",
        "vmnet", "veth", "cni", "flannel", "wg", NULL
    };
    int i;
    if (name == NULL || name[0] == '\0') {
        return 1;
    }
    for (i = 0; pref[i]; i++) {
        size_t n = strlen(pref[i]);
        if (strncmp(name, pref[i], n) == 0) {
            return 1;
        }
    }
    return 0;
}

static int skip_fstype(const char *t)
{
    static const char *skip[] = {
        "proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "cgroup", "cgroup2",
        "pstore", "bpf", "tracefs", "debugfs", "securityfs", "hugetlbfs",
        "mqueue", "fusectl", "configfs", "ramfs", "overlay", "squashfs",
        "autofs", "efivarfs", "rpc_pipefs", "fuse.gvfsd-fuse",
        "fuse.portal", "nsfs", "binfmt_misc", NULL
    };
    int i;
    if (t == NULL) {
        return 1;
    }
    for (i = 0; skip[i]; i++) {
        if (strcmp(t, skip[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int read_u64_file(const char *path, uint64_t *out)
{
    FILE *f = fopen(path, "r");
    unsigned long long v = 0;
    if (f == NULL) {
        return -1;
    }
    if (fscanf(f, "%llu", &v) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = (uint64_t)v;
    return 0;
}

static void fill_identity(ResourceSnapshot *out)
{
    struct utsname u;
    gethostname(out->hostname, sizeof(out->hostname) - 1);
    if (uname(&u) == 0) {
        snprintf(out->os_name, sizeof(out->os_name), "%s", u.sysname);
        snprintf(out->os_release, sizeof(out->os_release), "%s", u.release);
        snprintf(out->arch, sizeof(out->arch), "%s", u.machine);
    }
    snprintf(out->host_label, sizeof(out->host_label), "Linux");
}

static void apply_cpu_delta(CpuTicks *prev, uint64_t idle, uint64_t total,
                            double *usage)
{
    if (g_primed && total > prev->total) {
        uint64_t d_total = total - prev->total;
        uint64_t d_idle = idle - prev->idle;
        *usage = util_clamp_pct(100.0 * (double)(d_total - d_idle) / (double)d_total);
    } else {
        *usage = 0.0;
    }
    prev->idle = idle;
    prev->total = total;
}

static void fill_cpu(ResourceSnapshot *out)
{
    FILE *f;
    char line[512];
    int cores = 0;
    uint64_t all_idle = 0, all_total = 0;
    int got_all = 0;

    f = fopen("/proc/stat", "r");
    if (f == NULL) {
        out->cpu_total = -1.0;
        return;
    }

    /* WORKING: /proc/stat's first line is the aggregate of every cpuN
     * line. We use that for the headline bar, and the cpuN lines for
     * the per-core strip. Idle is idle+iowait (time the CPU was not
     * running a thread); steal is included in total so a busy
     * hypervisor still shows up. */
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long long user = 0, nice = 0, system = 0, idle = 0;
        unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
        unsigned long long guest = 0, guest_nice = 0;
        uint64_t idle_all, total;
        int n;

        if (strncmp(line, "cpu", 3) != 0) {
            break;
        }

        if (line[3] == ' ' || line[3] == '\t') {
            n = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &user, &nice, &system, &idle, &iowait, &irq, &softirq,
                       &steal, &guest, &guest_nice);
            if (n < 4) {
                continue;
            }
            idle_all = (uint64_t)idle + (uint64_t)iowait;
            total = (uint64_t)user + (uint64_t)nice + (uint64_t)system
                    + idle_all + (uint64_t)irq + (uint64_t)softirq
                    + (uint64_t)steal;
            all_idle = idle_all;
            all_total = total;
            got_all = 1;
            continue;
        }

        if (cores >= SNAP_MAX_CORES) {
            continue;
        }
        n = sscanf(line, "cpu%*d %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &system, &idle, &iowait, &irq, &softirq,
                   &steal, &guest, &guest_nice);
        if (n < 4) {
            continue;
        }
        idle_all = (uint64_t)idle + (uint64_t)iowait;
        total = (uint64_t)user + (uint64_t)nice + (uint64_t)system
                + idle_all + (uint64_t)irq + (uint64_t)softirq
                + (uint64_t)steal;
        snprintf(out->cores[cores].name, sizeof(out->cores[cores].name),
                 "%d", cores);
        apply_cpu_delta(&g_cpu_core[cores], idle_all, total,
                        &out->cores[cores].usage);
        cores++;
    }
    fclose(f);

    out->core_count = cores;
    out->cpu_count = cores > 0 ? cores : (int)sysconf(_SC_NPROCESSORS_ONLN);

    if (got_all) {
        double headline = 0.0;
        int was_primed = g_primed;
        apply_cpu_delta(&g_cpu_all, all_idle, all_total, &headline);
        out->cpu_total = was_primed ? headline : -1.0;
    } else {
        out->cpu_total = -1.0;
    }
}

static uint64_t meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    size_t keylen = strlen(key);
    uint64_t v = 0;
    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, key, keylen) == 0 && line[keylen] == ':') {
            unsigned long long kb = 0;
            if (sscanf(line + keylen + 1, "%llu", &kb) == 1) {
                v = (uint64_t)kb;
            }
            break;
        }
    }
    fclose(f);
    return v;
}

static void fill_memory(ResourceSnapshot *out)
{
    uint64_t total_kb = meminfo_kb("MemTotal");
    uint64_t avail_kb = meminfo_kb("MemAvailable");
    uint64_t free_kb = meminfo_kb("MemFree");
    uint64_t buffers_kb = meminfo_kb("Buffers");
    uint64_t cached_kb = meminfo_kb("Cached");
    uint64_t swap_total = meminfo_kb("SwapTotal");
    uint64_t swap_free = meminfo_kb("SwapFree");

    out->mem_total = total_kb * 1024ULL;
    /* WORKING: MemAvailable is the figure `free` and modern top use —
     * reclaimable cache plus free. On kernels older than 3.14 it is
     * missing; fall back to Free+Buffers+Cached. */
    if (avail_kb == 0) {
        avail_kb = free_kb + buffers_kb + cached_kb;
    }
    if (avail_kb > total_kb) {
        avail_kb = total_kb;
    }
    out->mem_available = avail_kb * 1024ULL;
    out->mem_used = out->mem_total - out->mem_available;
    out->swap_total = swap_total * 1024ULL;
    out->swap_used = (swap_total > swap_free ? swap_total - swap_free : 0) * 1024ULL;
}

static void fill_disks(ResourceSnapshot *out)
{
    FILE *f;
    struct mntent *m;
    char seen_dev[SNAP_MAX_DISKS][SNAP_PATH_LEN];
    int seen = 0;

    f = setmntent("/proc/mounts", "r");
    if (f == NULL) {
        f = setmntent("/etc/mtab", "r");
    }
    if (f == NULL) {
        return;
    }

    while ((m = getmntent(f)) != NULL && out->disk_count < SNAP_MAX_DISKS) {
        struct statvfs vs;
        uint64_t total, used;
        int j, dup = 0;

        if (skip_fstype(m->mnt_type)) {
            continue;
        }
        if (m->mnt_fsname == NULL || m->mnt_fsname[0] != '/') {
            continue;
        }
        for (j = 0; j < seen; j++) {
            if (strcmp(seen_dev[j], m->mnt_fsname) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            continue;
        }
        if (statvfs(m->mnt_dir, &vs) != 0 || vs.f_blocks == 0) {
            continue;
        }
        total = (uint64_t)vs.f_blocks * (uint64_t)vs.f_frsize;
        used = ((uint64_t)vs.f_blocks - (uint64_t)vs.f_bfree)
               * (uint64_t)vs.f_frsize;
        if (total == 0) {
            continue;
        }

        snprintf(out->disks[out->disk_count].mount,
                 sizeof(out->disks[out->disk_count].mount), "%s", m->mnt_dir);
        snprintf(out->disks[out->disk_count].fstype,
                 sizeof(out->disks[out->disk_count].fstype), "%s", m->mnt_type);
        out->disks[out->disk_count].total_bytes = total;
        out->disks[out->disk_count].used_bytes = used;
        if (seen < SNAP_MAX_DISKS) {
            snprintf(seen_dev[seen], sizeof(seen_dev[seen]), "%s", m->mnt_fsname);
            seen++;
        }
        out->disk_count++;
    }
    endmntent(f);
}

static void fill_net(ResourceSnapshot *out, double dt)
{
    FILE *f;
    char line[512];
    int i;

    f = fopen("/proc/net/dev", "r");
    if (f == NULL) {
        return;
    }
    /* two header lines */
    if (fgets(line, sizeof(line), f) == NULL ||
        fgets(line, sizeof(line), f) == NULL) {
        fclose(f);
        return;
    }

    out->net_count = 0;
    while (fgets(line, sizeof(line), f) != NULL &&
           out->net_count < SNAP_MAX_NETS) {
        char name[SNAP_NAME_LEN];
        unsigned long long rx = 0, tx = 0;
        char *colon;
        int prev = -1;

        colon = strchr(line, ':');
        if (colon == NULL) {
            continue;
        }
        *colon = '\0';
        sscanf(line, " %63s", name);
        if (skip_iface(name)) {
            continue;
        }
        if (sscanf(colon + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu",
                   &rx, &tx) != 2) {
            continue;
        }

        snprintf(out->nets[out->net_count].name,
                 sizeof(out->nets[out->net_count].name), "%s", name);
        out->nets[out->net_count].rx_bytes = (uint64_t)rx;
        out->nets[out->net_count].tx_bytes = (uint64_t)tx;
        out->nets[out->net_count].rx_bps = 0.0;
        out->nets[out->net_count].tx_bps = 0.0;

        if (g_primed && dt > 0.0) {
            for (i = 0; i < g_net_n; i++) {
                if (strcmp(g_net_name[i], name) == 0) {
                    prev = i;
                    break;
                }
            }
            if (prev >= 0) {
                if ((uint64_t)rx >= g_net_rx[prev]) {
                    out->nets[out->net_count].rx_bps =
                        (double)((uint64_t)rx - g_net_rx[prev]) / dt;
                }
                if ((uint64_t)tx >= g_net_tx[prev]) {
                    out->nets[out->net_count].tx_bps =
                        (double)((uint64_t)tx - g_net_tx[prev]) / dt;
                }
            }
        }
        out->net_count++;
    }
    fclose(f);

    g_net_n = out->net_count;
    for (i = 0; i < g_net_n; i++) {
        snprintf(g_net_name[i], sizeof(g_net_name[i]), "%s", out->nets[i].name);
        g_net_rx[i] = out->nets[i].rx_bytes;
        g_net_tx[i] = out->nets[i].tx_bytes;
    }
}

static void fill_load_uptime_procs(ResourceSnapshot *out)
{
    FILE *f;
    double load1 = 0, load5 = 0, load15 = 0;
    double up = 0;
    DIR *d;
    struct dirent *ent;
    uint32_t procs = 0;

    f = fopen("/proc/loadavg", "r");
    if (f != NULL) {
        if (fscanf(f, "%lf %lf %lf", &load1, &load5, &load15) == 3) {
            out->load1 = load1;
            out->load5 = load5;
            out->load15 = load15;
            out->load_valid = 1;
        }
        fclose(f);
    }

    f = fopen("/proc/uptime", "r");
    if (f != NULL) {
        if (fscanf(f, "%lf", &up) == 1 && up > 0.0) {
            out->uptime_sec = (uint64_t)up;
        }
        fclose(f);
    }

    /* WORKING: /proc/<pid> directories are the process list. loadavg's
     * "234" in 1/234 is threads, not processes, so we count pids. */
    d = opendir("/proc");
    if (d != NULL) {
        while ((ent = readdir(d)) != NULL) {
            char *end = NULL;
            if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
                continue;
            }
            (void)strtol(ent->d_name, &end, 10);
            if (end && *end == '\0') {
                procs++;
            }
        }
        closedir(d);
    }
    out->process_count = procs;
}

static void fill_battery(ResourceSnapshot *out)
{
    DIR *d;
    struct dirent *ent;

    d = opendir("/sys/class/power_supply");
    if (d == NULL) {
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        char path[256];
        char status[64];
        FILE *sf;
        uint64_t cap = 0;

        if (strncmp(ent->d_name, "BAT", 3) != 0) {
            continue;
        }
        snprintf(path, sizeof(path),
                 "/sys/class/power_supply/%s/capacity", ent->d_name);
        if (read_u64_file(path, &cap) != 0) {
            continue;
        }
        out->battery_present = 1;
        out->battery_percent = util_clamp_int((int)cap, 0, 100);

        snprintf(path, sizeof(path),
                 "/sys/class/power_supply/%s/status", ent->d_name);
        sf = fopen(path, "r");
        if (sf != NULL) {
            if (fgets(status, sizeof(status), sf) != NULL) {
                out->battery_charging = (strncmp(status, "Charging", 8) == 0);
                out->battery_plugged =
                    out->battery_charging ||
                    strncmp(status, "Full", 4) == 0 ||
                    strncmp(status, "Not charging", 12) == 0;
            }
            fclose(sf);
        }
        break;
    }
    closedir(d);
}

int collect_snapshot(ResourceSnapshot *out)
{
    double now, dt;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->cpu_total = -1.0;
    out->battery_percent = -1;

    now = util_now_sec();
    dt = (g_last_t > 0.0) ? (now - g_last_t) : 0.0;
    if (dt < 0.0) {
        dt = 0.0;
    }

    fill_identity(out);
    fill_cpu(out);
    fill_memory(out);
    fill_disks(out);
    fill_net(out, dt);
    fill_load_uptime_procs(out);
    fill_battery(out);

    out->sample_dt = dt;
    out->primed = g_primed;
    g_primed = 1;
    g_last_t = now;
    return 0;
}
