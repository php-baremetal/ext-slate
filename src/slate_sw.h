/* slate_sw.h - private to Slate: what the rasterizers (raster.c) use of the software graphics layer (gfx_sw.c)
   beyond slate_gfx.h. Pure C, no ESP-IDF and no PHP. */
#ifndef SLATE_SW_H
#define SLATE_SW_H

#include <stdbool.h>

#include "slate_config.h"
#include "slate_gfx.h"

#if SLATE_HAS_GRADIENTS
/* The pieces of one gradient row drawn one after the other with the same stops (a radial gradient's row):
   slate_sw_grow_piece(r, x, n, t0, dt, cov) writes exactly what gradient_row(b, x, y, n, t0, dt, stops, ns, cov)
   of the software table writes, with the work that only depends on the row and the stops done once by
   slate_sw_grow_begin. Only for a table whose gradient_row is the software one (slate_sw_gradient), on the
   compositor's task like every drawing call; false from begin: nothing to draw on that row. */
typedef struct {
    slate_buf_t *b;
    int y, ns;
    const slate_stop_t *stops;
    uint32_t gen;
} slate_sw_grow_t;

bool slate_sw_gradient(const slate_gfx_ops_t *g);
bool slate_sw_grow_begin(slate_sw_grow_t *r, slate_buf_t *b, int y, const slate_stop_t *stops, int ns);
void slate_sw_grow_piece(const slate_sw_grow_t *r, int x, int n, float t0, float dt, const uint8_t *cov);
#endif

#endif
