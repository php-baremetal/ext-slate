/* dirty.h - Slate's dirty regions: the screen rectangles that must be composed again in the next frame.
   Pure C, no ESP-IDF and no PHP.

   At most SLATE_DIRTY_MAX rectangles, screen coordinates, half open, never overlapping. Every pixel of every
   rectangle added lies in one of them. slate_dirty_add():
   - clips the rectangle to the screen [0, screen_w) x [0, screen_h); an empty result is ignored;
   - rounds x outward to even columns (x0 down, x1 up, x1 capped at screen_w): RGB565 pixels then go in pairs
     (32-bit fills, swaps and copies stay aligned) and panels that want even column windows get them;
   - ignores it when a rectangle of the set already contains it;
   - merges it with a rectangle of the set when their bounding box costs at most SLATE_DIRTY_COST pixels more than
     the two (the pixels of the box that neither covers), and the box meets no other rectangle; the box is looked
     at again against the others. A region costs about that much on its own (the panel's window and transfer set
     up, every element touching it set up again), so merging pays from there;
   - otherwise, when it overlaps a rectangle, only the parts outside it (up to four rectangles) are added;
   - when the set is full, the two rectangles (the new one included) whose bounding box costs the least become
     that box (with every rectangle it then meets).
   Many small changes far apart (dots moving across the screen) stay many small regions; neighbours merge.
   The set is consumed by slate_compose(), which leaves it empty. */
#ifndef SLATE_DIRTY_H
#define SLATE_DIRTY_H

#include "slate_gfx.h"

#define SLATE_DIRTY_MAX 32
#define SLATE_DIRTY_COST 1024   /* pixels: see above */

typedef struct { slate_rect_t r[SLATE_DIRTY_MAX]; int n; } slate_dirty_t;

void slate_dirty_add(slate_dirty_t *d, slate_rect_t r, int screen_w, int screen_h);
static inline void slate_dirty_clear(slate_dirty_t *d) { d->n = 0; }

static inline int slate_rect_empty(slate_rect_t r) { return r.x0 >= r.x1 || r.y0 >= r.y1; }
/* Intersection (may come out empty) and union (an empty operand is ignored). */
slate_rect_t slate_rect_and(slate_rect_t a, slate_rect_t b);
slate_rect_t slate_rect_or(slate_rect_t a, slate_rect_t b);
/* a contains b (an empty b is contained in anything). */
int slate_rect_contains(slate_rect_t a, slate_rect_t b);

#endif
