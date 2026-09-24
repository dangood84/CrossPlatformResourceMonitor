#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

void     util_sleep_ms(int ms);
double   util_now_sec(void);
int      util_clamp_int(int v, int lo, int hi);
double   util_clamp_pct(double v);

/* 1024-based, labelled KB/MB/GB so the numbers match Activity Monitor /
 * Task Manager more closely than SI (1000-based) units. */
void     util_format_bytes(uint64_t bytes, char *buf, size_t n);
void     util_format_rate(double bps, char *buf, size_t n);
void     util_format_uptime(uint64_t seconds, char *buf, size_t n);

int      util_nocolor_requested(void);

#endif /* UTIL_H */
