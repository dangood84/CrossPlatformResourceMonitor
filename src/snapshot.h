#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <stdint.h>

/* WORKING: one portable struct is the whole contract between a host
 * collector and the renderer. Darwin, Linux, and Windows fill the same
 * fields; nothing in render.c includes a Mach or Win32 header. */

#define SNAP_MAX_CORES  128
#define SNAP_MAX_DISKS    8
#define SNAP_MAX_NETS    12
#define SNAP_NAME_LEN    64
#define SNAP_PATH_LEN   128
#define SNAP_HOST_LEN   256

typedef struct {
    char     name[SNAP_NAME_LEN];
    double   usage;            /* 0..100 */
} CoreSample;

typedef struct {
    char     mount[SNAP_PATH_LEN];
    char     fstype[SNAP_NAME_LEN];
    uint64_t total_bytes;
    uint64_t used_bytes;
} DiskSample;

typedef struct {
    char     name[SNAP_NAME_LEN];
    uint64_t rx_bytes;         /* lifetime counters, for debugging */
    uint64_t tx_bytes;
    double   rx_bps;           /* bytes/sec since previous sample */
    double   tx_bps;
} NetSample;

typedef struct {
    char     hostname[SNAP_HOST_LEN];
    char     os_name[SNAP_NAME_LEN];
    char     os_release[SNAP_NAME_LEN];
    char     arch[SNAP_NAME_LEN];
    char     host_label[SNAP_NAME_LEN]; /* "macOS" / "Linux" / "Windows" */

    int      cpu_count;        /* logical processors */
    double   cpu_total;        /* 0..100; -1 if not primed yet */
    CoreSample cores[SNAP_MAX_CORES];
    int      core_count;

    uint64_t mem_total;
    uint64_t mem_used;
    uint64_t mem_available;
    uint64_t swap_total;
    uint64_t swap_used;

    DiskSample disks[SNAP_MAX_DISKS];
    int      disk_count;

    NetSample nets[SNAP_MAX_NETS];
    int      net_count;

    double   load1, load5, load15;
    int      load_valid;       /* 0 on Windows: no kernel load average */

    uint64_t uptime_sec;
    uint32_t process_count;

    int      battery_present;
    int      battery_percent;  /* 0..100, or -1 if unknown */
    int      battery_charging;
    int      battery_plugged;  /* AC adapter attached */

    double   sample_dt;        /* seconds between the two counter reads */
    int      primed;           /* 1 once CPU/net rates are meaningful */
} ResourceSnapshot;

#endif /* SNAPSHOT_H */
