#ifndef RENDER_H
#define RENDER_H

#include "snapshot.h"

typedef struct {
    int ascii;          /* '#' / '.' instead of block glyphs */
    int color;          /* ANSI colour; honour NO_COLOR in monitor.c */
    int paused;
    double interval_sec;
    int cols;           /* terminal width, already clamped */
} RenderOptions;

void render_dashboard(const ResourceSnapshot *snap, const RenderOptions *opt);
void render_once(const ResourceSnapshot *snap, const RenderOptions *opt);

#endif /* RENDER_H */
