#include "collect.h"
#include "util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <iphlpapi.h>
#ifdef __MINGW32__
#include <ifdef.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Per-core times. Documented as SystemProcessorPerformanceInformation (8). */
typedef struct {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG InterruptCount;
} SysProcPerf;

typedef LONG NTSTATUS;
typedef NTSTATUS (WINAPI *NtQuerySystemInformation_fn)(ULONG, PVOID, ULONG, PULONG);

#define SystemProcessorPerformanceInformation 8

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
static NtQuerySystemInformation_fn g_ntqsi;

const char *collect_host_name(void)
{
    return "Windows";
}

static void copy_trunc(char *dst, size_t n, const char *src)
{
    /* WORKING: MinGW's -Wformat-truncation fires on snprintf("%s") when
     * the source is a slice of a bigger buffer (GetLogicalDriveStrings
     * is 512 bytes; mount is 128). The real strings are "C:\\". This
     * copy is explicit about the cap and does not go through printf. */
    if (dst == NULL || n == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    {
        size_t len = strlen(src);
        if (len >= n) {
            len = n - 1;
        }
        memcpy(dst, src, len);
        dst[len] = '\0';
    }
}

int collect_init(void)
{
    HMODULE ntdll;
    memset(&g_cpu_all, 0, sizeof(g_cpu_all));
    memset(g_cpu_core, 0, sizeof(g_cpu_core));
    g_net_n = 0;
    g_primed = 0;
    g_last_t = 0.0;
    /* WORKING: NtQuerySystemInformation is the documented way to get
     * per-CPU idle/kernel/user times. It lives in ntdll; we look it up
     * so a missing export falls back to GetSystemTimes (one bar).
     *
     * GetProcAddress returns FARPROC. MinGW -Wcast-function-type will
     * not accept a direct cast to our prototype (different return
     * width on x64). A union is the usual way to re-type the pointer
     * without that warning. */
    ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll != NULL) {
        FARPROC raw = GetProcAddress(ntdll, "NtQuerySystemInformation");
        if (raw != NULL) {
            union {
                FARPROC raw;
                NtQuerySystemInformation_fn fn;
            } u;
            u.raw = raw;
            g_ntqsi = u.fn;
        }
    }
    return 0;
}

void collect_shutdown(void)
{
}

static uint64_t filetime_u64(const FILETIME *ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    return (uint64_t)u.QuadPart;
}

static void fill_identity(ResourceSnapshot *out)
{
    DWORD n = (DWORD)(sizeof(out->hostname) - 1);
    SYSTEM_INFO si;
    GetComputerNameA(out->hostname, &n);
    snprintf(out->os_name, sizeof(out->os_name), "Windows");
    snprintf(out->host_label, sizeof(out->host_label), "Windows");
#ifdef _WIN64
    snprintf(out->arch, sizeof(out->arch), "x86_64");
#else
    snprintf(out->arch, sizeof(out->arch), "x86");
#endif
    GetNativeSystemInfo(&si);
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) {
        snprintf(out->arch, sizeof(out->arch), "arm64");
    } else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) {
        snprintf(out->arch, sizeof(out->arch), "x86_64");
    }
    {
        OSVERSIONINFOA v;
        memset(&v, 0, sizeof(v));
        v.dwOSVersionInfoSize = sizeof(v);
        /* WORKING: GetVersionEx is the compatibility-shimmed API; it
         * may report 6.2 on newer Windows unless the manifest claims
         * otherwise. The dashboard only needs a hint, not a SKU. */
        if (GetVersionExA(&v)) {
            snprintf(out->os_release, sizeof(out->os_release), "%u.%u.%u",
                     (unsigned)v.dwMajorVersion,
                     (unsigned)v.dwMinorVersion,
                     (unsigned)v.dwBuildNumber);
        }
    }
}

static void apply_delta(CpuTicks *prev, uint64_t idle, uint64_t total, double *usage)
{
    if (g_primed && total > prev->total) {
        uint64_t d_total = total - prev->total;
        uint64_t d_idle = idle >= prev->idle ? idle - prev->idle : 0;
        if (d_idle > d_total) {
            d_idle = d_total;
        }
        *usage = util_clamp_pct(100.0 * (double)(d_total - d_idle) / (double)d_total);
    } else {
        *usage = 0.0;
    }
    prev->idle = idle;
    prev->total = total;
}

static void fill_cpu(ResourceSnapshot *out)
{
    SYSTEM_INFO si;
    FILETIME idle_ft, kernel_ft, user_ft;
    uint64_t idle, kernel, user, total;
    double headline = 0.0;

    GetSystemInfo(&si);
    out->cpu_count = (int)si.dwNumberOfProcessors;

    /* GetSystemTimes: KernelTime includes IdleTime. */
    if (GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) {
        idle = filetime_u64(&idle_ft);
        kernel = filetime_u64(&kernel_ft);
        user = filetime_u64(&user_ft);
        total = kernel + user;
        apply_delta(&g_cpu_all, idle, total, &headline);
        out->cpu_total = g_primed ? headline : -1.0;
    } else {
        out->cpu_total = -1.0;
    }

    if (g_ntqsi != NULL && out->cpu_count > 0) {
        ULONG ret = 0;
        int n = out->cpu_count;
        size_t bytes;
        SysProcPerf *info;
        int i;

        if (n > SNAP_MAX_CORES) {
            n = SNAP_MAX_CORES;
        }
        bytes = (size_t)n * sizeof(SysProcPerf);
        info = (SysProcPerf *)malloc(bytes);
        if (info != NULL &&
            g_ntqsi(SystemProcessorPerformanceInformation, info,
                    (ULONG)bytes, &ret) >= 0) {
            int got = (int)(ret / sizeof(SysProcPerf));
            if (got > n) {
                got = n;
            }
            out->core_count = got;
            for (i = 0; i < got; i++) {
                uint64_t c_idle = (uint64_t)info[i].IdleTime.QuadPart;
                uint64_t c_kernel = (uint64_t)info[i].KernelTime.QuadPart;
                uint64_t c_user = (uint64_t)info[i].UserTime.QuadPart;
                uint64_t c_total = c_kernel + c_user;
                snprintf(out->cores[i].name, sizeof(out->cores[i].name), "%d", i);
                apply_delta(&g_cpu_core[i], c_idle, c_total, &out->cores[i].usage);
            }
        }
        free(info);
    }

    if (out->core_count == 0 && out->cpu_count > 0) {
        out->core_count = 1;
        snprintf(out->cores[0].name, sizeof(out->cores[0].name), "0");
        out->cores[0].usage = out->cpu_total > 0.0 ? out->cpu_total : 0.0;
    }
}

static void fill_memory(ResourceSnapshot *out)
{
    MEMORYSTATUSEX m;
    memset(&m, 0, sizeof(m));
    m.dwLength = sizeof(m);
    if (!GlobalMemoryStatusEx(&m)) {
        return;
    }
    out->mem_total = (uint64_t)m.ullTotalPhys;
    out->mem_available = (uint64_t)m.ullAvailPhys;
    if (out->mem_available > out->mem_total) {
        out->mem_available = out->mem_total;
    }
    out->mem_used = out->mem_total - out->mem_available;
    /* WORKING: Windows has no single "swap file used" number that
     * matches Unix swap. Page-file commit (TotalPageFile - Avail)
     * minus physical is a usable stand-in for "extra backing store". */
    if (m.ullTotalPageFile > m.ullTotalPhys) {
        out->swap_total = (uint64_t)(m.ullTotalPageFile - m.ullTotalPhys);
        if (m.ullAvailPageFile < m.ullTotalPageFile) {
            uint64_t committed = (uint64_t)(m.ullTotalPageFile - m.ullAvailPageFile);
            out->swap_used = committed > out->mem_used
                ? committed - out->mem_used : 0;
            if (out->swap_used > out->swap_total) {
                out->swap_used = out->swap_total;
            }
        }
    }
}

static void fill_disks(ResourceSnapshot *out)
{
    char drives[512];
    char *d;
    if (GetLogicalDriveStringsA((DWORD)sizeof(drives), drives) == 0) {
        return;
    }
    for (d = drives; *d != '\0' && out->disk_count < SNAP_MAX_DISKS;
         d += strlen(d) + 1) {
        ULARGE_INTEGER free_avail, total, total_free;
        UINT type = GetDriveTypeA(d);
        if (type != DRIVE_FIXED && type != DRIVE_RAMDISK) {
            continue;
        }
        if (!GetDiskFreeSpaceExA(d, &free_avail, &total, &total_free)) {
            continue;
        }
        if (total.QuadPart == 0) {
            continue;
        }
        copy_trunc(out->disks[out->disk_count].mount,
                   sizeof(out->disks[out->disk_count].mount), d);
        copy_trunc(out->disks[out->disk_count].fstype,
                   sizeof(out->disks[out->disk_count].fstype), "ntfs");
        out->disks[out->disk_count].total_bytes = (uint64_t)total.QuadPart;
        out->disks[out->disk_count].used_bytes =
            (uint64_t)(total.QuadPart - total_free.QuadPart);
        out->disk_count++;
    }
}

static int skip_iface(const char *name)
{
    static const char *needles[] = {
        "Loopback", "Teredo", "isatap", "Pseudo", "Bluetooth",
        "Virtual", "Hyper-V", "vEthernet", NULL
    };
    int i;
    if (name == NULL || name[0] == '\0') {
        return 1;
    }
    for (i = 0; needles[i]; i++) {
        if (strstr(name, needles[i]) != NULL) {
            return 1;
        }
    }
    return 0;
}

static void fill_net(ResourceSnapshot *out, double dt)
{
    DWORD size = 0;
    MIB_IFTABLE *table;
    DWORD i;

    if (GetIfTable(NULL, &size, TRUE) != ERROR_INSUFFICIENT_BUFFER) {
        return;
    }
    table = (MIB_IFTABLE *)malloc(size);
    if (table == NULL) {
        return;
    }
    if (GetIfTable(table, &size, TRUE) != NO_ERROR) {
        free(table);
        return;
    }

    out->net_count = 0;
    for (i = 0; i < table->dwNumEntries && out->net_count < SNAP_MAX_NETS; i++) {
        MIB_IFROW *row = &table->table[i];
        char name[SNAP_NAME_LEN];
        uint64_t rx, tx;
        int prev = -1;
        int k;

        if (row->dwType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        copy_trunc(name, sizeof(name), (const char *)row->bDescr);
        if (skip_iface(name)) {
            continue;
        }
        /* WORKING: MIB_IFROW counters are 32-bit and wrap at 4 GiB.
         * Rates over a 1s interval still work; a wrap is treated as a
         * reset (rate 0 for that tick) rather than a huge spike. */
        rx = (uint64_t)row->dwInOctets;
        tx = (uint64_t)row->dwOutOctets;

        snprintf(out->nets[out->net_count].name,
                 sizeof(out->nets[out->net_count].name), "%s", name);
        out->nets[out->net_count].rx_bytes = rx;
        out->nets[out->net_count].tx_bytes = tx;
        out->nets[out->net_count].rx_bps = 0.0;
        out->nets[out->net_count].tx_bps = 0.0;

        if (g_primed && dt > 0.0) {
            for (k = 0; k < g_net_n; k++) {
                if (strcmp(g_net_name[k], name) == 0) {
                    prev = k;
                    break;
                }
            }
            if (prev >= 0) {
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
    free(table);

    g_net_n = out->net_count;
    for (i = 0; i < (DWORD)g_net_n; i++) {
        snprintf(g_net_name[i], sizeof(g_net_name[i]), "%s", out->nets[i].name);
        g_net_rx[i] = out->nets[i].rx_bytes;
        g_net_tx[i] = out->nets[i].tx_bytes;
    }
}

static void fill_uptime_procs(ResourceSnapshot *out)
{
    DWORD pids[4096];
    DWORD needed = 0;
    out->uptime_sec = GetTickCount64() / 1000ULL;
    if (EnumProcesses(pids, sizeof(pids), &needed)) {
        out->process_count = needed / sizeof(DWORD);
    }
}

static void fill_battery(ResourceSnapshot *out)
{
    SYSTEM_POWER_STATUS s;
    if (!GetSystemPowerStatus(&s)) {
        return;
    }
    /* 128 = no system battery. 255 = unknown percentage. */
    if (s.BatteryFlag == 128) {
        return;
    }
    out->battery_present = 1;
    out->battery_percent = (s.BatteryLifePercent == 255)
        ? -1 : (int)s.BatteryLifePercent;
    out->battery_plugged = (s.ACLineStatus == 1);
    out->battery_charging = (s.BatteryFlag & 8) != 0;
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
    out->load_valid = 0; /* Windows has no kernel load average. */

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
    fill_uptime_procs(out);
    fill_battery(out);

    out->sample_dt = dt;
    out->primed = g_primed;
    g_primed = 1;
    g_last_t = now;
    return 0;
}
