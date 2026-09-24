#ifndef COLLECT_H
#define COLLECT_H

#include "snapshot.h"

/* Each OS compiles exactly one collect_*.c, the same idea as the Pascal
 * projects' {$IFDEF} host units. The function names stay identical so
 * monitor.c never mentions Darwin or Win32. */

int         collect_init(void);
void        collect_shutdown(void);
int         collect_snapshot(ResourceSnapshot *out);
const char *collect_host_name(void);

#endif /* COLLECT_H */
