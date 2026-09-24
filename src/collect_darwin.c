#include "collect.h"
#include "util.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>

#include <libproc.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* WORKING: CPU and network rates are deltas. The first collect_snapshot
 * only stores tick/byte counters; the second (and later) turns those
 * into percentages and bytes/sec. monitor.c always primes once. */
typedef struct {
    uint64_t idle;
    uint64_t total;
} CpuTicks;

static CpuTicks g_cpu_all;
static CpuTicks g_cpu_core[SNAP_MAX_CORES];
static int      g_cpu_n;
static uint64_t g_net_rx[SNAP_MAX_NETS];
static uint64_t g_net_tx[SNAP_MAX_NETS];
static char     g_net_name[SNAP_MAX_NETS][SNAP_NAME_LEN];
static int      g_net_n;
static int      g_primed;
static double   g_last_t;

const char *collect_host_name(void)
{
    return "macOS";
}

int collect_init(void)
{
    memset(&g_cpu_all, 0, sizeof(g_cpu_all));
    memset(g_cpu_core, 0, sizeof(g_cpu_core));
    g_cpu_n = 0;
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
    /* WORKING: macOS advertises a lot of virtual NICs (utun for VPN,
     * awdl for AirDrop, llw, anpi, XHC). Counting their bytes would
     * make the dashboard look busy without being useful. */
    static const char *pref[] = {
        "lo", "utun", "awdl", "llw", "gif", "stf", "bridge",
        "p2p", "ap", "anpi", "XHC", "vmnet", "vmenet", "llw",
        "sipc", "ipsec", NULL
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

static void fill_identity(ResourceSnapshot *out)
{
    char ostype[64] = {0};
    char osrel[64] = {0};
    char machine[64] = {0};
    size_t n;

    memset(out->hostname, 0, sizeof(out->hostname));
    gethostname(out->hostname, sizeof(out->hostname) - 1);

    n = sizeof(ostype);
    sysctlbyname("kern.ostype", ostype, &n, NULL, 0);
    n = sizeof(osrel);
    sysctlbyname("kern.osrelease", osrel, &n, NULL, 0);
    n = sizeof(machine);
    sysctlbyname("hw.machine", machine, &n, NULL, 0);

    snprintf(out->os_name, sizeof(out->os_name), "%s",
             ostype[0] ? ostype : "Darwin");
    snprintf(out->os_release, sizeof(out->os_release), "%s", osrel);
    snprintf(out->arch, sizeof(out->arch), "%s", machine);
    snprintf(out->host_label, sizeof(out->host_label), "macOS");
}

static void fill_cpu(ResourceSnapshot *out, double dt)
{
    natural_t count = 0;
    processor_info_array_t info = NULL;
    mach_msg_type_number_t info_count = 0;
    processor_cpu_load_info_t cpu;
    kern_return_t kr;
    natural_t i;
    uint64_t all_idle = 0;
    uint64_t all_total = 0;

    kr = host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                             &count, &info, &info_count);
    if (kr != KERN_SUCCESS || info == NULL || count == 0) {
        out->cpu_count = 0;
        out->core_count = 0;
        out->cpu_total = -1.0;
        return;
    }

    cpu = (processor_cpu_load_info_t)info;
    if (count > SNAP_MAX_CORES) {
        count = SNAP_MAX_CORES;
    }
    out->cpu_count = (int)count;
    out->core_count = (int)count;

    for (i = 0; i < count; i++) {
        uint64_t user = cpu[i].cpu_ticks[CPU_STATE_USER];
        uint64_t sys  = cpu[i].cpu_ticks[CPU_STATE_SYSTEM];
        uint64_t nice = cpu[i].cpu_ticks[CPU_STATE_NICE];
        uint64_t idle = cpu[i].cpu_ticks[CPU_STATE_IDLE];
        uint64_t total = user + sys + nice + idle;
        uint64_t d_idle, d_total;

        snprintf(out->cores[i].name, sizeof(out->cores[i].name), "%u", i);
        if (g_primed && total > g_cpu_core[i].total) {
            d_total = total - g_cpu_core[i].total;
            d_idle = idle - g_cpu_core[i].idle;
            if (d_total > 0) {
                out->cores[i].usage = util_clamp_pct(
                    100.0 * (double)(d_total - d_idle) / (double)d_total);
            } else {
                out->cores[i].usage = 0.0;
            }
        } else {
            out->cores[i].usage = 0.0;
        }
        g_cpu_core[i].idle = idle;
        g_cpu_core[i].total = total;
        all_idle += idle;
        all_total += total;
    }

    if (g_primed && all_total > g_cpu_all.total) {
        uint64_t d_total = all_total - g_cpu_all.total;
        uint64_t d_idle = all_idle - g_cpu_all.idle;
        out->cpu_total = util_clamp_pct(
            100.0 * (double)(d_total - d_idle) / (double)d_total);
    } else {
        out->cpu_total = -1.0;
    }
    g_cpu_all.idle = all_idle;
    g_cpu_all.total = all_total;
    g_cpu_n = (int)count;

    (void)dt;
    vm_deallocate(mach_task_self(), (vm_address_t)info,
                  info_count * sizeof(integer_t));
}

static void fill_memory(ResourceSnapshot *out)
{
    int64_t memsize = 0;
    size_t n = sizeof(memsize);
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    uint64_t used;
    struct xsw_usage swap;
    size_t slen;

    sysctlbyname("hw.memsize", &memsize, &n, NULL, 0);
    out->mem_total = (uint64_t)memsize;

    host_page_size(mach_host_self(), &page);
    memset(&vm, 0, sizeof(vm));
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vm, &count) != KERN_SUCCESS) {
        return;
    }

    /* WORKING: Activity Monitor's "Memory Used" is roughly
     *   (internal - purgeable) + wired + compressed
     * not "total - free". Free pages on macOS are a small leftover;
     * inactive/file-backed pages are reclaimable and should not count
     * as pressure the way they do on a naive Linux MemFree read. */
    used = ((uint64_t)vm.internal_page_count - (uint64_t)vm.purgeable_count
            + (uint64_t)vm.wire_count
            + (uint64_t)vm.compressor_page_count) * (uint64_t)page;
    if (used > out->mem_total) {
        used = out->mem_total;
    }
    out->mem_used = used;
    out->mem_available = out->mem_total - used;

    memset(&swap, 0, sizeof(swap));
    slen = sizeof(swap);
    if (sysctlbyname("vm.swapusage", &swap, &slen, NULL, 0) == 0) {
        out->swap_total = (uint64_t)swap.xsu_total;
        out->swap_used = (uint64_t)swap.xsu_used;
    }
}

static void fill_disks(ResourceSnapshot *out)
{
    struct statfs *mnt = NULL;
    int n;
    int i;
    char seen_dev[SNAP_MAX_DISKS][SNAP_PATH_LEN];
    int seen = 0;

    n = getmntinfo(&mnt, MNT_NOWAIT);
    if (n <= 0 || mnt == NULL) {
        return;
    }

    for (i = 0; i < n && out->disk_count < SNAP_MAX_DISKS; i++) {
        uint64_t total, used;
        int j;
        int dup = 0;

        /* WORKING: APFS firmlinks mean / and /System/Volumes/Data are
         * the same store. MNT_DONTBROWSE hides Preboot/VM/Update.
         * Deduping on f_mntfromname keeps the dashboard to real volumes. */
        if (mnt[i].f_blocks == 0) {
            continue;
        }
        if (mnt[i].f_flags & MNT_DONTBROWSE) {
            continue;
        }
        if (!(mnt[i].f_flags & MNT_LOCAL)) {
            continue;
        }
        if (strcmp(mnt[i].f_fstypename, "devfs") == 0 ||
            strcmp(mnt[i].f_fstypename, "autofs") == 0) {
            continue;
        }
        for (j = 0; j < seen; j++) {
            if (strcmp(seen_dev[j], mnt[i].f_mntfromname) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            continue;
        }

        total = (uint64_t)mnt[i].f_blocks * (uint64_t)mnt[i].f_bsize;
        used = ((uint64_t)mnt[i].f_blocks - (uint64_t)mnt[i].f_bfree)
               * (uint64_t)mnt[i].f_bsize;
        if (total == 0) {
            continue;
        }

        snprintf(out->disks[out->disk_count].mount,
                 sizeof(out->disks[out->disk_count].mount),
                 "%s", mnt[i].f_mntonname);
        snprintf(out->disks[out->disk_count].fstype,
                 sizeof(out->disks[out->disk_count].fstype),
                 "%s", mnt[i].f_fstypename);
        out->disks[out->disk_count].total_bytes = total;
        out->disks[out->disk_count].used_bytes = used;

        if (seen < SNAP_MAX_DISKS) {
            snprintf(seen_dev[seen], sizeof(seen_dev[seen]),
                     "%s", mnt[i].f_mntfromname);
            seen++;
        }
        out->disk_count++;
    }
}

static void fill_net(ResourceSnapshot *out, double dt)
{
    struct ifaddrs *ifa = NULL;
    struct ifaddrs *p;
    int i;

    if (getifaddrs(&ifa) != 0) {
        return;
    }

    out->net_count = 0;
    for (p = ifa; p != NULL && out->net_count < SNAP_MAX_NETS; p = p->ifa_next) {
        const struct if_data *data;
        const char *name = p->ifa_name;
        uint64_t rx, tx;
        int prev = -1;

        if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_LINK) {
            continue;
        }
        if (skip_iface(name)) {
            continue;
        }
        data = (const struct if_data *)p->ifa_data;
        if (data == NULL) {
            continue;
        }
        rx = (uint64_t)data->ifi_ibytes;
        tx = (uint64_t)data->ifi_obytes;

        snprintf(out->nets[out->net_count].name,
                 sizeof(out->nets[out->net_count].name), "%s", name);
        out->nets[out->net_count].rx_bytes = rx;
        out->nets[out->net_count].tx_bytes = tx;
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
                /* Counters are monotonic; a reset (interface recreated)
                 * would underflow, so only compute a rate when they grew. */
                if (rx >= g_net_rx[prev]) {
                    out->nets[out->net_count].rx_bps =
                        (double)(rx - g_net_rx[prev]) / dt;
                }
                if (tx >= g_net_tx[prev]) {
                    out->nets[out->net_count].tx_bps =
                        (double)(tx - g_net_tx[prev]) / dt;
                }
            }
        }
        out->net_count++;
    }
    freeifaddrs(ifa);

    g_net_n = out->net_count;
    for (i = 0; i < g_net_n; i++) {
        snprintf(g_net_name[i], sizeof(g_net_name[i]), "%s", out->nets[i].name);
        g_net_rx[i] = out->nets[i].rx_bytes;
        g_net_tx[i] = out->nets[i].tx_bytes;
    }
}

static void fill_load_uptime_procs(ResourceSnapshot *out)
{
    double load[3];
    struct timeval boot;
    size_t n;
    int bufsize;
    time_t now;

    if (getloadavg(load, 3) == 3) {
        out->load1 = load[0];
        out->load5 = load[1];
        out->load15 = load[2];
        out->load_valid = 1;
    }

    memset(&boot, 0, sizeof(boot));
    n = sizeof(boot);
    if (sysctlbyname("kern.boottime", &boot, &n, NULL, 0) == 0) {
        now = time(NULL);
        if (now > boot.tv_sec) {
            out->uptime_sec = (uint64_t)(now - boot.tv_sec);
        }
    }

    /* WORKING: proc_listpids asks the kernel for the pid list size, then
     * we divide by sizeof(pid_t). A race (a process dying mid-call) can
     * undershoot by one; that is fine for a dashboard. */
    bufsize = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (bufsize > 0) {
        out->process_count = (uint32_t)(bufsize / (int)sizeof(pid_t));
    }
}

static void fill_battery(ResourceSnapshot *out)
{
    CFTypeRef blob;
    CFArrayRef list;
    CFIndex i, n;

    blob = IOPSCopyPowerSourcesInfo();
    if (blob == NULL) {
        return;
    }
    list = IOPSCopyPowerSourcesList(blob);
    if (list == NULL) {
        CFRelease(blob);
        return;
    }

    n = CFArrayGetCount(list);
    for (i = 0; i < n; i++) {
        CFDictionaryRef desc;
        CFStringRef type;
        CFNumberRef cur, maxn;
        CFBooleanRef charging, present;
        int cur_i = 0, max_i = 0;

        desc = IOPSGetPowerSourceDescription(blob, CFArrayGetValueAtIndex(list, i));
        if (desc == NULL) {
            continue;
        }
        type = CFDictionaryGetValue(desc, CFSTR(kIOPSTypeKey));
        if (type == NULL ||
            CFStringCompare(type, CFSTR(kIOPSInternalBatteryType), 0)
                != kCFCompareEqualTo) {
            continue;
        }
        present = CFDictionaryGetValue(desc, CFSTR(kIOPSIsPresentKey));
        if (present != NULL && !CFBooleanGetValue(present)) {
            continue;
        }

        out->battery_present = 1;
        cur = CFDictionaryGetValue(desc, CFSTR(kIOPSCurrentCapacityKey));
        maxn = CFDictionaryGetValue(desc, CFSTR(kIOPSMaxCapacityKey));
        if (cur && maxn &&
            CFNumberGetValue(cur, kCFNumberIntType, &cur_i) &&
            CFNumberGetValue(maxn, kCFNumberIntType, &max_i) &&
            max_i > 0) {
            out->battery_percent = util_clamp_int((cur_i * 100) / max_i, 0, 100);
        } else {
            out->battery_percent = -1;
        }
        charging = CFDictionaryGetValue(desc, CFSTR(kIOPSIsChargingKey));
        out->battery_charging = charging && CFBooleanGetValue(charging);

        {
            CFStringRef state = CFDictionaryGetValue(desc, CFSTR(kIOPSPowerSourceStateKey));
            out->battery_plugged = state != NULL &&
                CFStringCompare(state, CFSTR(kIOPSACPowerValue), 0)
                    == kCFCompareEqualTo;
        }
        break;
    }

    CFRelease(list);
    CFRelease(blob);
}

int collect_snapshot(ResourceSnapshot *out)
{
    double now;
    double dt;

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
    fill_cpu(out, dt);
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
