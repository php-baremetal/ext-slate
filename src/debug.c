/* Slate's profiler: see debug.h. */
#include "debug.h"

#if SLATE_DEBUG
#include <stdio.h>
#include <string.h>

void slate_debug_init(slate_debug_t *d, int64_t now_us)
{
    memset(d, 0, sizeof *d);
    d->start_us = now_us;
}

void slate_debug_cmds(slate_debug_t *d, uint32_t n)
{
    d->cmds += n;
}

void slate_debug_frame(slate_debug_t *d, uint32_t frame_us, uint32_t compose_us, uint32_t wait_us, int rects,
                       const slate_stats_t *st, uint32_t budget_us, const char *scene)
{
    d->frames++;
    d->frame_us += frame_us;
    d->compose_us += compose_us;
    d->wait_us += wait_us;
    d->area += st->area;
    d->rects += (uint32_t)(rects > 0 ? rects : 0);
    d->drawn += st->drawn;
    d->culled += st->culled;
    d->hit += st->cache_hit;
    d->miss += st->cache_miss;
    if (frame_us > budget_us && !d->warned) {
        d->warned = true;
        d->over_us = frame_us;
        snprintf(d->over_scene, sizeof d->over_scene, "%s", scene && *scene ? scene : "(no scene)");
    }
}

bool slate_debug_over(slate_debug_t *d, char *line, size_t cap)
{
    if (!d->over_us) {
        return false;
    }
    snprintf(line, cap, "slate: frame %.1f ms over budget in %s", d->over_us / 1000.0, d->over_scene);
    d->over_us = 0;
    return true;
}

/* Rounded mean per frame. */
static uint32_t avg(uint64_t sum, uint32_t n)
{
    return n ? (uint32_t)((sum + n / 2) / n) : 0;
}

bool slate_debug_tick(slate_debug_t *d, int64_t now_us, uint32_t elements, uint32_t screen_px, float *fps,
                      char *line, size_t cap)
{
    if (now_us < slate_debug_due(d)) {
        return false;
    }
    int64_t span = now_us - d->start_us;
    *fps = d->frames * 1e6f / (float)(span > 0 ? span : 1);
    {
        uint32_t n = d->frames;   /* 0: a second without frames, every average 0 */
        double pct = screen_px && n ? 100.0 * (double)d->area / ((double)screen_px * n) : 0.0;
        snprintf(line, cap,
                 "slate: %.1f fps | frame %.1f ms (compose %.1f, wait %.1f) | dirty %u%% in %u rects | "
                 "%u elements, %u drawn, %u culled | cache %u hit %u miss | cmds %u",
                 *fps, n ? d->frame_us / 1000.0 / n : 0.0, n ? d->compose_us / 1000.0 / n : 0.0,
                 n ? d->wait_us / 1000.0 / n : 0.0,
                 (unsigned)(pct + 0.5), (unsigned)avg(d->rects, n), (unsigned)elements, (unsigned)avg(d->drawn, n),
                 (unsigned)avg(d->culled, n), (unsigned)avg(d->hit, n), (unsigned)avg(d->miss, n),
                 (unsigned)d->cmds);
    }
    slate_debug_init(d, now_us);
    return true;
}
#else
typedef int slate_debug_unused_t;   /* ISO C: a translation unit is not empty */
#endif
