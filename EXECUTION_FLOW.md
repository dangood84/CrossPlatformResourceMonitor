# Execution flow: from `main` to a drawn CPU bar

A step-by-step trace of what happens from `int main` through host initialisation, down to how two Mach (or `/proc`, or Win32) counter reads become `CPU  16.8%` on the dashboard.

Default launch (`make run`) opens the **live macOS dashboard**. `make linux` / `make windows` use the same `monitor.c` and `render.c`; only the collect step changes. This trace is **macOS** (`collect_darwin.c`) unless a step says otherwise.

One thread does everything:

- **main** — flags, prime, sleep, collect, paint, poll

There is no GUI thread and no worker. `collect_snapshot` and `printf` run on the same stack that called `main`.

---

## Phase A — process entry

**1.** The OS loads `build/resource-monitor`. C runtime initialises. Static collector buffers (`g_cpu_all`, `g_net_rx`, …) are zero.

**2.** `main` walks `argv`. No flags means: live mode, 1.0 s interval, Unicode bars, colour unless `NO_COLOR` is set.

```c
/* src/monitor.c */
if (collect_init() != 0) ...
collect_snapshot(&snap);                 /* step 5 */
util_sleep_ms((int)(interval * 1000.0)); /* step 6 */
collect_snapshot(&snap);                 /* step 10 */
```

**3.** `signal(SIGINT)` / `SIGTERM` point at `on_stop`, which only sets `g_stop`. The handler does **not** call `term_restore` — `term_init` has not run yet, and doing TTY work in a handler is unsafe. `atexit` will restore if we got as far as `term_init`.

**4.** `collect_init` (Darwin) zeroes the previous-tick structs and sets `g_primed = 0`.

Windows: `collect_init` also `GetProcAddress`s `NtQuerySystemInformation` from `ntdll.dll` so a missing export falls back to a single `GetSystemTimes` bar.

Linux: `collect_init` is the same zeroing; the files are opened later, per snapshot.

---

## Phase B — prime (first snapshot, rates invalid)

**5.** `collect_snapshot` `memset`s the out-struct, then:

| Call | What it reads | What the dashboard would show |
|------|----------------|-------------------------------|
| `fill_identity` | `gethostname`, `kern.ostype`, `kern.osrelease`, `hw.machine` | `Daniels-MacBook-Pro-2.local  ·  macOS 23.6.0  ·  arm64` |
| `fill_cpu` | `host_processor_info` tick counters | `cpu_total = -1`, `primed = 0` |
| `fill_memory` | `hw.memsize` + `HOST_VM_INFO64` | used / total, already honest |
| `fill_disks` | `getmntinfo`, skip `MNT_DONTBROWSE`, dedupe `f_mntfromname` | `/` on APFS |
| `fill_net` | `getifaddrs` `AF_LINK`, skip `utun`/`awdl`/… | lifetime bytes, B/s = 0 |
| `fill_load_uptime_procs` | `getloadavg`, `kern.boottime`, `proc_listpids` | load / up / nprocs |
| `fill_battery` | `IOPSCopyPowerSourcesInfo` | percent + AC/charging |

**6.** The function stores this tick's CPU idle/total and each NIC's byte counters in static `g_*` buffers, sets `g_primed = 1` for **next** time, and returns. The snapshot the caller holds still has `primed == 0` (the value *before* that assignment).

**7.** `util_sleep_ms(1000)`. `clock_gettime(CLOCK_MONOTONIC)` is not involved in the sleep itself; `nanosleep` is. If `SIGWINCH` arrives, the remainder is slept. Testers: do not expect the prime to be shorter than the interval unless the process is killed.

Windows prime uses `Sleep`. Linux is the same `nanosleep`.

---

## Phase C — second snapshot (first honest frame)

**8.** `collect_snapshot` again. `dt = now - g_last_t` (about 1.00 s).

**9.** `fill_cpu` reads Mach ticks a second time. For each core, and for the sum:

```
d_total = this_total - g_cpu_core[i].total
d_idle  = this_idle  - g_cpu_core[i].idle
usage   = 100 * (d_total - d_idle) / d_total
```

`cpu_total` is the same formula on the summed counters, **not** the average of the per-core bars (they match on a quiet machine; they can differ slightly because of rounding).

**10.** `fill_net` subtracts `ifi_ibytes` / `ifi_obytes` and divides by `dt`. A NIC that appeared since the last tick has no previous name match → rate 0 this frame, real rate on the next.

**11.** Memory / disks / battery are levels, not rates. They are just read again. They were already valid on the first snapshot.

**12.** The snapshot now has `primed = 1`, `sample_dt ≈ 1.0`, `cpu_total` in `0..100`.

Linux at this step is `/proc/stat` `cpu` + `cpuN` lines (`idle+iowait` as idle). Windows is `GetSystemTimes` for the headline and `NtQuerySystemInformation(SystemProcessorPerformanceInformation)` for the strip. Kernel time on Windows **includes** idle; the collector subtracts it.

---

## Phase D — paint (`--once` or first live frame)

**13.** `--once`, `make once`, or a non-TTY stdout: `render_once` → `paint` → process exits. Terminal mode is unchanged. This is what `make test` is *not* — the test binary never calls `render_*` at all; it only asserts ranges and prints one `ok` line.

**14.** Live TTY: `term_init`:

```
tcgetattr → clear ICANON, ECHO; keep ISIG
\x1b[?1049h     enter alternate screen
\x1b[?25l       hide cursor
atexit(term_restore)
```

Windows: `ENABLE_VIRTUAL_TERMINAL_PROCESSING`, UTF-8 `SetConsoleOutputCP`, console input without line-echo.

**15.** Loop body:

```
term_size → opt.cols          /* TIOCGWINSZ, clamped 60..240 */
if (!paused) collect_snapshot
render_dashboard              /* \x1b[H\x1b[J + paint */
key = term_poll_key(1000)
```

**16.** `paint` writes, in order:

1. Title + optional `PAUSED`
2. Hostname · host label · release · arch · core count
3. Headline CPU bar (`cpu_total`) and a wrapped per-core strip
4. Memory bar, swap bar (or `none`)
5. One disk line per remaining volume
6. One net line per NIC that has ever seen a byte (or every NIC if none have)
7. Load (Unix) + processes + uptime
8. Battery, if `battery_present`
9. Footer: `q` / `space` / interval / local clock

Bar fill is `round(pct / 100 * width)`. Colour: green `< 60`, yellow `< 85`, red otherwise. `--ascii` uses `#` / `.`.

**17.** `term_poll_key`: `poll(stdin, interval_ms)`.

| Byte | Action |
|------|--------|
| `q` / `Q` / `0x03` | break the loop |
| space | toggle `paused` |
| `ESC` + CSI (arrows) | drained, ignored |
| none (timeout) | next collect + frame |

`ISIG` is still on, so Ctrl+C is usually a **signal**, not `0x03`. `on_stop` sets `g_stop`; the loop notices after `poll` returns (`EINTR` or timeout).

---

## The repeating loop

```text
main thread
  → collect_snapshot          (Mach / /proc / Win32)
        fill_* write ResourceSnapshot
  → render_dashboard
        term_begin_frame (\x1b[H\x1b[J)
        paint bars + numbers
  → term_poll_key(interval)
        q → restore → exit
        space → paused := !paused
        timeout → next tick
```

Quit: `q` → `term_restore` (`\x1b[?25h\x1b[?1049l` + `tcsetattr` old) → `collect_shutdown` → `return 0`. The previous scrollback is back. Ctrl+C is the same path via `g_stop`.

Pause after step 16: further iterations skip `collect_snapshot`. The bars freeze. The footer clock still updates because `paint` calls `time()` every frame.

---

## Keyboard path (no model.Press)

There is no key-to-enum table like the calculator. Three bytes matter: `q`, space, and the signal for Ctrl+C. Everything else, including UTF-8 paste, is ignored (UTF-8 continuation bytes are not commands).

Resize: no `SIGWINCH` handler. The next frame calls `term_size` again. A tester who wants a 100-column layout should resize, wait one interval, and look at `opt.cols`.

---

## Windows path (same draw loop)

`main` is identical. Differences that matter when tracing:

1. `collect_win.c` — `GetSystemTimes` / `NtQuerySystemInformation` / `GlobalMemoryStatusEx` / `GetLogicalDriveStrings` / `GetIfTable` / `GetTickCount64` / `EnumProcesses` / `GetSystemPowerStatus`.
2. `load_valid` stays 0; `paint` omits the Load columns.
3. `term_poll_key` is `_kbhit` + `Sleep(20)` in a `GetTickCount` window. Arrow keys arrive as `0`/`224` + a follow-up; both are swallowed.
4. 32-bit `dwInOctets` wrap → rate 0 for that tick (see WORKINGS.md).

Close the console window: the process dies; `atexit` still runs `term_restore` if `term_init` ran.

---

## Linux path (same draw loop)

1. `/proc/stat` first line = headline CPU; `cpu0`… = strip.
2. `/proc/meminfo` `MemAvailable` (or Free+Buffers+Cached).
3. `/proc/mounts` + `statvfs`, skip virtual fstypes, dedupe `mnt_fsname`.
4. `/proc/net/dev` after two header lines.
5. `/proc/loadavg`, `/proc/uptime`, count `/proc/[0-9]*`.
6. First `BAT*` under `/sys/class/power_supply`.

`make linux` on a Mac will compile `collect_linux.c` and then fail at run time if `/proc` is missing. Run that target **on Linux**.

---

## One-line map

`main` → `collect_init` → **snapshot (store counters)** → **sleep** → **snapshot (subtract)** → `term_init` → **collect → paint → poll** → `term_restore`.

Debugger: `main`, `collect_snapshot`, `fill_cpu`, `render_dashboard`, `term_poll_key`, `term_restore`. The first *visible* live frame is already the second sample; there is no "warming up" line unless you breakpoint before the prime sleep and force a paint.

See also `WORKINGS.md` for who owns which field and the Activity Monitor / `MemAvailable` / page-file definitions in more detail.
