# Cross-Platform Resource Monitor — console dashboard (C99)
#
# macOS:   make
# Linux:   make            (or: make linux)
# Windows: from MinGW/MSVC: make windows
#
# Same idea as the Pascal projects: one tree, compile-time host.

CC       ?= gcc
SRC      := src
BUILD    := build
CFLAGS   ?= -std=c99 -Wall -Wextra -O2 -Isrc
LDFLAGS  ?=

UNAME_S  := $(shell uname -s 2>/dev/null || echo Unknown)

ifeq ($(UNAME_S),Darwin)
  COLLECT  := $(SRC)/collect_darwin.c
  LDLIBS   := -framework IOKit -framework CoreFoundation
endif
ifeq ($(UNAME_S),Linux)
  COLLECT  := $(SRC)/collect_linux.c
  LDLIBS   :=
endif
ifneq (,$(findstring MINGW,$(UNAME_S)))
  COLLECT  := $(SRC)/collect_win.c
  LDLIBS   := -lpsapi -liphlpapi
endif
ifneq (,$(findstring MSYS,$(UNAME_S)))
  COLLECT  := $(SRC)/collect_win.c
  LDLIBS   := -lpsapi -liphlpapi
endif

COMMON   := $(SRC)/util.c $(SRC)/term.c $(SRC)/render.c $(COLLECT)

.PHONY: all run linux windows test once clean

all: $(BUILD)/resource-monitor

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/resource-monitor: $(BUILD) $(SRC)/*.c $(SRC)/*.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/monitor.c $(COMMON) $(LDFLAGS) $(LDLIBS)

$(BUILD)/monitortest: $(BUILD) $(SRC)/*.c $(SRC)/*.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/monitortest.c $(SRC)/util.c $(COLLECT) $(LDFLAGS) $(LDLIBS)

run: $(BUILD)/resource-monitor
	$(BUILD)/resource-monitor

once: $(BUILD)/resource-monitor
	$(BUILD)/resource-monitor --once

test: $(BUILD)/monitortest
	$(BUILD)/monitortest

# Explicit host targets (run these on that OS; they do not cross-compile).
linux: $(BUILD)
	$(CC) $(CFLAGS) -o $(BUILD)/resource-monitor \
	    $(SRC)/monitor.c $(SRC)/util.c $(SRC)/term.c $(SRC)/render.c \
	    $(SRC)/collect_linux.c

windows: $(BUILD)
	$(CC) $(CFLAGS) -o $(BUILD)/ResourceMonitor.exe \
	    $(SRC)/monitor.c $(SRC)/util.c $(SRC)/term.c $(SRC)/render.c \
	    $(SRC)/collect_win.c -lpsapi -liphlpapi

clean:
	rm -rf $(BUILD)
