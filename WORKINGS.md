# How the Resource Monitor works

This note is for an automation tester who wants to see how a small C console app is structured: where it starts, who owns state, who paints the dashboard, and how two kernel counter reads become `CPU 16.8%`.

You do not need to be a Mach, `/proc`, or Win32 expert. The same ideas show up in many monitors: an entry point, a snapshot struct, a host that fills it, and a loop that sleeps then redraws.

There is **no ncurses**, **no Pascal host unit**, and **no Swing**. Each metric is a field on `ResourceSnapshot`. A collector that cannot fill a field leaves it zero (or `-1` for "not yet"). The renderer prints what it is given.

This is a **timed loop**, not a GUI event loop like Goody's Calculator. Nothing interesting happens until the interval elapses (or a key arrives). That is the whole difference from the desk apps.

## Mental model

```
monitor.c main
  → collect_init                  # one of collect_darwin / linux / win
  → collect_snapshot              # prime: store counters, rates are junk
  → sleep(interval)
  → collect_snapshot              # now CPU % and B/s are real
  → if --once or not a TTY: print once, exit
  → term_init (alt screen, raw stdin)
       loop
           collect_snapshot       # unless paused
           render_dashboard
           term_poll_key(interval)
           q / Ctrl+C → restore terminal, exit
```

| Layer | File | Tester-friendly analogy |
|-------|------|-------------------------|
| Entry / flags | `monitor.c` | Test runner: `--once`, `--interval`, `--ascii` |
| State | `snapshot.h` | Fixture: one struct, no OS types |
| Host | `collect_*.c` | The only file that opens `/proc` or Mach |
| View | `render.c` | Turns the struct into bars and numbers |
| Terminal shell | `term.c` | Alt screen + "did the user hit q?" |

Only **one** `collect_*.c` is linked. The other two are not compiled on that OS. Same compile-time host idea as `uhostcocoa` / `uhostwin` / `uhostgtk`.

---

## 1. Entry point and execution lifecycle

### Where `main` lives

The process entry point is `main` in `src/monitor.c`. It parses flags (no `getopt`, so the same file builds with MSVC / MinGW), calls `collect_init`, takes **two** snapshots, then either prints once or enters the live loop.

```c
collect_snapshot(&snap);          /* counters only */
util_sleep_ms(interval_ms);
collect_snapshot(&snap);          /* rates exist */
if (once || !term_is_tty())
    render_once(&snap, &opt);
else {
    term_init();
    while (!g_stop) { ... }
}
```

### Lifecycle, step by step (macOS — the reference host)

1. **The OS** starts `build/resource-monitor`.
2. **`main`** reads `argv`. Default interval is 1.0 s. `NO_COLOR` forces `--no-color`.
3. **`collect_init`** (Darwin) zeroes the previous-tick buffers. Nothing talks to Mach yet.
4. **First `collect_snapshot`** fills hostname, memory, disks, battery immediately. CPU `cpu_total` is `-1` and `primed` is 0 — there is no previous tick to subtract.
5. **`util_sleep_ms`** waits one interval. `nanosleep` is restarted on `EINTR` so a stray signal does not shorten the prime.
6. **Second `collect_snapshot`** subtracts Mach tick counters and interface byte counters. `primed` is 1. `sample_dt` is the real elapsed time (about 1.0 s, or 0.25 s under `make test`).
7. Live mode: **`term_init`** switches the TTY to non-canonical input, hides the cursor, and enters the alternate screen. `atexit` + `SIGINT` both call `term_restore`.
8. Each loop iteration: optional collect, `render_dashboard` (`\x1b[H\x1b[J` + paint), `term_poll_key` for up to one interval.
9. **`q`**, Ctrl+C, or `SIGTERM`: restore the previous screen and cooked input, then `collect_shutdown`.

### Two user journeys

**`--once` / `make once` / piped stdout:**

```
prime → sleep → snapshot → render_once → exit 0
```

No `tcsetattr`. Safe to grep. This is the path a CI job should use.

**Live dashboard (`make run`):**

```
prime → sleep → snapshot → term_init → (collect → paint → poll)* → restore
```

`space` flips `paused`. Collection stops; the last snapshot is redrawn so the footer can still show `PAUSED`.

### Why testers care

- **Compile-time host is the feature flag.** Automating "Mac numbers" vs "Linux numbers" is `make` vs `make linux` on that machine, not a CLI switch.
- **The first snapshot is a fixture, not a result.** A test that asserts `cpu_total >= 0` after one call will fail on purpose. `monitortest.c` takes two.
- **Rates are interval-dependent.** `--interval 5` does not change the percentage formula (it is still `busy_delta / total_delta`) but it does change network B/s smoothness.
- **Exit is process-level.** There is no daemon and no pid file. Closing the terminal sends `SIGHUP`/`SIGINT` and `term_restore` runs from `atexit` if we got that far.
- **`make test`** never opens the alt screen. Use that for "do we have a hostname and a non-zero `mem_total`". Use the live window for colour, pause, and resize.

---

## 2. Main files and responsibilities

This is a **separation of snapshot vs paint**, not a framework.

### `monitor.c` — composition root

- Parses flags
- Primes the collector
- Owns the pause flag and the stop signal
- Does **not** read `/proc` or Mach itself

### `snapshot.h` — the contract

Holds *numbers*, not file descriptors:

- `cpu_total` / `cores[]` — 0..100, or `cpu_total == -1` before prime
- `mem_total` / `mem_used` / `mem_available`
- `swap_total` / `swap_used`
- `disks[]` — mount, fstype, used/total
- `nets[]` — name, lifetime bytes, B/s
- `load1/5/15` + `load_valid` (0 on Windows)
- `uptime_sec`, `process_count`
- `battery_*`
- `primed`, `sample_dt`

Hosts never poke the renderer. The renderer never pokes the host.

### `collect_darwin.c` / `collect_linux.c` / `collect_win.c`

Each file exports the same four symbols (`collect_init`, `collect_shutdown`, `collect_snapshot`, `collect_host_name`). Static buffers remember the previous tick.

That is why `monitortest.c` can link `util.c` + one collector and never mention ANSI.

### `render.c` — the dashboard

- Picks bar width from `opt->cols` (from `TIOCGWINSZ` / `GetConsoleScreenBufferInfo`)
- Green < 60%, yellow < 85%, red otherwise
- Hides NICs whose lifetime counters are still zero, unless *every* NIC is zero
- `--ascii` swaps `█░` for `#.`

It does not know about Mach or `/proc`.

### `term.c` — the shell

- Unix: clear `ICANON` and `ECHO`, keep `ISIG` so Ctrl+C still raises `SIGINT`
- Windows: `ENABLE_VIRTUAL_TERMINAL_PROCESSING` + UTF-8 output code page
- `term_poll_key` is a `poll` / `_kbhit` with a timeout — that timeout **is** the refresh interval

### What is *not* a file

There is no config file, no preferences plist, no log. Digit widths and colour thresholds are constants in `render.c`. A test that wants a red CPU bar has to actually load the machine; there is no `--fake-cpu=90`.

---

## 3. How counters become a percentage

This is **not** "read a `%` from the kernel". The kernel gives **monotonic counters**. The host subtracts.

```
this_total - prev_total  = ticks (or bytes) in the interval
this_idle  - prev_idle   = idle ticks in the interval
busy / total * 100       = utilisation
byte_delta / sample_dt   = bytes/sec
```

### CPU (all three hosts)

| Host | Counter source | Idle definition |
|------|----------------|-----------------|
| macOS | `host_processor_info(PROCESSOR_CPU_LOAD_INFO)` | `CPU_STATE_IDLE` |
| Linux | `/proc/stat` `cpu` / `cpuN` | `idle + iowait` |
| Windows | `GetSystemTimes` + `NtQuerySystemInformation(8)` | `IdleTime`; kernel time **includes** idle |

A guest steal tick on Linux is part of `total`, so a busy hypervisor still shows up.

`cpu_total == -1` is the "warming up" signal. After prime it is clamped to 0..100.

### Memory

| Host | "Used" means |
|------|----------------|
| macOS | `(internal - purgeable) + wired + compressor` × page size. **Not** `hw.memsize - free_count`. Free pages on Darwin are a leftover; inactive file-backed pages are reclaimable. This is the Activity Monitor "Memory Used" shape. |
| Linux | `MemTotal - MemAvailable` (kB × 1024). `MemAvailable` is the `free(1)` figure. Pre-3.14 kernels fall back to `Free + Buffers + Cached`. |
| Windows | `ullTotalPhys - ullAvailPhys` from `GlobalMemoryStatusEx`. Swap is approximated as page-file commit beyond physical. |

If a tester compares the bar to Activity Monitor / Task Manager and they disagree by a gigabyte, check which definition that tool is using (cached vs wired vs compressed) before filing a bug.

### Disks

Local, browseable volumes only. Deduped on device name so APFS firmlinks (`/` and `/System/Volumes/Data`) appear once. `MNT_DONTBROWSE` hides Preboot / VM / Update. Linux skips `tmpfs`, `proc`, `cgroup`, `overlay`, and anything whose `mnt_fsname` does not start with `/`.

Used = `(blocks - bfree) * bsize`. That counts root-reserved space as used, which matches `df` without `-B`.

### Network

Lifetime byte counters (`ifi_ibytes` / `/proc/net/dev` / `dwInOctets`) divided by `sample_dt`. Virtual NICs (`utun`, `awdl`, `docker`, `veth`, Loopback, Teredo, …) are dropped in the collector. Remaining ports with **zero lifetime bytes** are dropped in the renderer.

Windows `GetIfTable` counters are 32-bit and wrap at 4 GiB. A wrap is treated as a reset (rate 0 for that tick) rather than a multi-GB/s spike.

### Load, uptime, processes, battery

| Metric | macOS | Linux | Windows |
|--------|-------|-------|---------|
| Load | `getloadavg` | `/proc/loadavg` | omitted (`load_valid = 0`) |
| Uptime | `now - kern.boottime` | `/proc/uptime` | `GetTickCount64 / 1000` |
| Processes | `proc_listpids` size | count of `/proc/[0-9]*` | `EnumProcesses` |
| Battery | IOKit `IOPSCopyPowerSourcesInfo` | `/sys/class/power_supply/BAT*` | `GetSystemPowerStatus` (128 = none) |

`proc_listpids` can undershoot by one if a process dies mid-call. Fine for a dashboard.

---

## 4. The live loop vs `--once`

Two kinds of state matter:

- **Collector statics** — previous tick counters. Survive across snapshots inside one process. Lost on exit.
- **Render options** — ascii / colour / paused / interval. Never written by the collector.

### When it starts and stops

| Hook | Meaning |
|------|--------|
| `collect_init` | Zero previous-tick buffers |
| first `collect_snapshot` | Identity + levels; rates invalid |
| sleep + second snapshot | First honest frame |
| `term_init` | Alt screen (live only) |
| `term_poll_key` | Sleep that can be interrupted by `q` / space |
| `term_restore` / `atexit` | Cooked input, cursor on, previous scrollback |

### Update vs draw (important split)

| Function | Mutates | Draws |
|----------|---------|-------|
| `collect_snapshot` | snapshot + previous-tick statics | no |
| `render_dashboard` | nothing in the snapshot | yes |
| `term_poll_key` | nothing | no |
| space key | `paused` in `main` | no (next paint shows `PAUSED`) |

A tester debugging "CPU is always 0" should breakpoint the second `collect_snapshot` and watch `g_primed`. A tester debugging "the window is blank" should breakpoint `render_dashboard` / `term_begin_frame`. A tester debugging "q does nothing" should breakpoint `term_poll_key` and check that `ICANON` is off. A tester debugging "my shell is broken after Ctrl+C" should breakpoint `term_restore`.

### Why not `sleep(1)` then `scanf`?

A blocking `fgets` would make the dashboard freeze until Enter. A blocking `sleep` would ignore `q` until the interval ended. `poll` with a timeout is both the timer and the keyboard.

---

## Quick map of files

```
src/
  monitor.c           # main, flags, prime, loop
  snapshot.h          # ResourceSnapshot
  collect.h           # host API
  collect_darwin.c    # Mach / sysctl / IOKit
  collect_linux.c     # /proc / sysfs
  collect_win.c       # Win32 / PDH-free
  render.c            # bars, colours, idle-NIC filter
  term.c              # alt screen, raw, poll
  util.c              # sleep, KiB labels, NO_COLOR
  monitortest.c       # make test
```

If you are tracing in a debugger, put breakpoints on `main`, `collect_snapshot`, `fill_cpu` (in the host file), `render_dashboard`, and `term_restore`. You will see: **prime → sleep → delta → paint → poll**.

See also `EXECUTION_FLOW.md` for a numbered walk of the first frame on macOS.
