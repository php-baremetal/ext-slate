/* debug.h - Slate's profiler (built only with the slate-debug option, SLATE_DEBUG in slate_config.h; without it
   this header declares nothing and debug.c compiles to nothing). Pure C, no ESP-IDF and no PHP.

   The engine feeds it every frame it composes for the scene (frames that only redraw the FPS overlay are left
   out) and the commands it applies; once a second (slate_debug_tick) it gives the line the engine prints:
     slate: 58.9 fps | frame 9.8 ms (compose 6.1, wait 3.7) | dirty 31% in 3 rects | 42 elements, 17 drawn,
     9 culled | cache 2 hit 0 miss | cmds 12
   (one line): fps over the second; per frame averages of the time from the start of the frame to its last band
   handed over (compose = drawing, wait = blocked on the panel: free band buffers and accepted pushes), of the
   share of the screen composed, of the dirty rectangles and of the compositor's drawn, culled, cache hit and
   miss counts; elements alive in the engine; commands applied in the second. A second without frames prints its
   line too ("slate: 0.0 fps | frame 0.0 ms ..."): a still screen shows that it costs nothing. A frame longer than the budget
   (1000 / refresh rate ms) gives "slate: frame <ms> ms over budget in <scene>", at most once a second.
   Nothing here prints: the engine prints the lines after the frame it is composing has handed over its last band, outside
   the frame's timings, so the console (slow, and blocking) does not delay a frame or count in it.
   Engine task only. */
#ifndef SLATE_DEBUG_H
#define SLATE_DEBUG_H

#include "slate_config.h"

#if SLATE_DEBUG
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compose.h"

#define SLATE_DEBUG_PERIOD_US 1000000

typedef struct {
    int64_t start_us;           /* the second being measured started here */
    uint32_t frames, rects, drawn, culled, hit, miss, cmds;
    uint64_t frame_us, compose_us, wait_us, area;
    bool warned;                /* an over-budget line was given in this second */
    uint32_t over_us;           /* a frame over budget whose line is still to print (0: none) */
    char over_scene[48];
} slate_debug_t;

void slate_debug_init(slate_debug_t *d, int64_t now_us);
void slate_debug_cmds(slate_debug_t *d, uint32_t n);
/* One frame of the scene: times in microseconds, the dirty rectangles it started with, the compositor's stats.
   Keeps the over-budget line (frame_us > budget_us, at most once a second) naming scene, for slate_debug_over. */
void slate_debug_frame(slate_debug_t *d, uint32_t frame_us, uint32_t compose_us, uint32_t wait_us, int rects,
                       const slate_stats_t *st, uint32_t budget_us, const char *scene);
/* The over-budget line kept by slate_debug_frame, into line (once); false when there is none. */
bool slate_debug_over(slate_debug_t *d, char *line, size_t cap);
/* When the second is over: writes the statistics line into line, *fps, starts
   the next second and returns true; false before. */
bool slate_debug_tick(slate_debug_t *d, int64_t now_us, uint32_t elements, uint32_t screen_px, float *fps,
                      char *line, size_t cap);
/* When the current second ends. */
static inline int64_t slate_debug_due(const slate_debug_t *d) { return d->start_us + SLATE_DEBUG_PERIOD_US; }
#endif

#endif
