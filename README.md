# Cross-Platform Resource Monitor

A live **console dashboard** for CPU, memory, swap, disks, network rates, load, uptime, process count, and battery.

Written in **C99**. There is no JVM, no curses library, and no GUI toolkit. Each host (macOS, Linux, Windows) compiles exactly one collector; the dashboard only ever sees a portable `ResourceSnapshot`. ANSI colour and an alternate-screen buffer are the whole "UI".

C is a better fit here than Pascal or Java for the same reason those languages were a better fit for Eyes and the text editors: the interesting work is talking to the kernel. Mach `host_statistics64`, `/proc/stat`, and `GetSystemTimes` are C APIs. One compile-time host (`collect_darwin.c` / `collect_linux.c` / `collect_win.c`) keeps that noise out of the renderer, the same way `{$IFDEF}` kept Cocoa / Win32 / GTK out of the calculator model.

The monitor:

- draws a full-screen dashboard that refreshes once a second (configurable)
- colours bars green / yellow / red at 60% and 85%
- needs **two samples** before CPU and network rates mean anything (the first tick only stores counters)
- treats a pipe or `--once` as a single snapshot, not a live loop
- honours `NO_COLOR` and `--ascii` for terminals that still mangle UTF-8
- quits on `q` or Ctrl+C without leaving the terminal in raw mode

How the pieces fit together (same style as Eyes, Goody's Calculator, and the Java editors): `WORKINGS.md` for responsibilities and the counter math, `EXECUTION_FLOW.md` for a tick-by-tick trace.

## Requirements

- A **C99 compiler** on your `PATH` (`cc` / `gcc` / `clang` / MinGW)

macOS (Xcode command-line tools, already enough):

```bash
xcode-select --install   # only if `cc` is missing
```

Debian / Raspberry Pi OS (build **on** the Pi, not from macOS):

```bash
sudo apt install build-essential
```

`make` or `make linux` both work there. glibc hides POSIX APIs under strict `-std=c99`; `src/posix_features.h` asks for them (`sigaction`, `nanosleep`, `gethostname`).

Windows: MinGW-w64, MSYS2, or any `gcc` that can see `windows.h`, `psapi`, and `iphlpapi`.

## Run

From the project root:

```bash
make
make run
```

That compiles to `build/resource-monitor` and opens the live dashboard. `q` quits; space pauses.

Or with Make on other OSes:

```bash
make linux      # Linux binary (run this on Linux)
make windows    # ResourceMonitor.exe (run this on Windows / MinGW)
make test       # headless two-sample checks (no raw terminal)
make once       # one framed snapshot, then exit
make clean      # remove build/
```

Manual compile on macOS:

```bash
mkdir -p build
cc -std=c99 -Wall -Wextra -O2 -Isrc -o build/resource-monitor \
    src/monitor.c src/util.c src/term.c src/render.c src/collect_darwin.c \
    -framework IOKit -framework CoreFoundation
build/resource-monitor
```

## Using it

1. `make run`. The first visible frame is already primed (the process slept one interval before drawing).
2. Watch the headline CPU bar and the per-core strip. They are deltas, not instantaneous `/proc` snapshots.
3. Memory is "in use" the way Activity Monitor / `free` / Task Manager mean it — not "total minus the free-page counter".
4. Network lines are **bytes/sec since the previous tick**. Idle Thunderbolt ports with zero lifetime counters are hidden.
5. `space` freezes collection (the clock in the footer still updates). `q` or Ctrl+C restores the terminal and exits.

## Flags

| Flag | Effect |
|------|--------|
| `-1` / `--once` | Print one snapshot and exit. No alt-screen, no raw mode. |
| `-n` / `--interval SEC` | Refresh interval (default `1.0`, clamped to `0.2`–`60`) |
| `--ascii` | `#` / `.` bars instead of `█` / `░` |
| `--no-color` | No ANSI colour. Also honours a non-empty `NO_COLOR`. |
| `-h` / `--help` | Usage text |

Piped to a file or to `less`, the process behaves like `--once` even without the flag. A live loop in a non-TTY is the wrong tool.

## Where it appears

| OS | Collector | What you see |
|----|-----------|--------------|
| **macOS** | `collect_darwin.c` | Mach CPU ticks, Activity Monitor-style memory, `getifaddrs`, IOKit battery |
| **Linux** | `collect_linux.c` | `/proc/stat`, `/proc/meminfo` (`MemAvailable`), `/proc/net/dev`, sysfs battery |
| **Windows** | `collect_win.c` | `GetSystemTimes` + `NtQuerySystemInformation`, `GlobalMemoryStatusEx`, `GetIfTable` |

There is no load average on Windows (the kernel does not have one). The line is omitted; process count and uptime stay.

## Project layout

```
src/
  monitor.c           # program; parse flags, prime, live loop
  snapshot.h          # portable ResourceSnapshot (the whole contract)
  collect.h           # collect_init / collect_snapshot
  collect_darwin.c    # macOS host
  collect_linux.c     # Linux host
  collect_win.c       # Windows host
  render.c            # ANSI dashboard
  term.c              # raw mode, alt screen, key poll
  util.c              # sleep, byte/rate/uptime formatting
  monitortest.c       # make test
Makefile
```
