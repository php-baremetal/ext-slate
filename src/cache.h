/* cache.h - Slate's element caches and offscreen buffers. Pure C, no ESP-IDF and no PHP.

   Offscreen buffers. A group composed offscreen (opacity < 1, cache, shadow or blur) is drawn into an RGB565 + A8
   image with premultiplied colours, the format copy_a takes (slate_gfx.h). slate_bufa_t is such an image; its
   slate_buf_t comes first, so the rasterizers draw into it as into any buffer, through slate_a8_ops(): the
   active graphics table for the colour (premultiplied "over" uses the same per-channel formula as the straight
   blend onto an opaque pixel) plus the update of the alpha plane, A' = a + A * (255 - a) / 255 for every pixel
   written with alpha a. The image starts as all zeros (transparent black).

   Caches. Memory comes from the target's cache_alloc (PSRAM on the board, slate_compose; the engine's takes the room
   of the scenes left first, engine.h, Kept scenes) and goes back to its cache_free; the record itself is a small struct from the scene allocator (slate_scene_set_alloc) that remembers the free function, so slate_el_free
   can release it. Two images per element:
   - content (GROUP with cache, shadow or blur): the group's children composed offscreen (and blurred when the
     group has a blur), in screen pixels around the group's origin;
   - shadow (ROUNDRECT, CIRCLE, GROUP with a shadow): A8, the coverage (or the content's alpha) blurred, drawn
     with fill_mask in the shadow colour.
   A scroll (scene.h, "Scroll") with cache and without shadow or blur keeps instead the image of its whole content:
   its children composed at position 0 over slate_el_scroll_area (crel, in content coordinates: the children's box,
   cut to the view's span on an axis the scroll cannot move), the same format; the caches of those children are built
   for that image, not cut to the screen. A change of its position, view or bars keeps it (each frame copies the part
   in the view) while the area needed stays inside it; a change inside rebuilds it. The children's own images made
   for the build are dropped once it is done (they are in it). A build in which a child's shadow or blur gets no
   memory is given up (memory freed, the scroll composed directly) and not tried again until its content changes.
   With a shadow or blur a scroll is drawn through the image of its view, as a group, rebuilt when it scrolls.
   Validity. An image is rebuilt on the next frame that needs it when:
   - something inside changed: any property of a descendant, a child added or removed, a canvas inside marked
     dirty (slate_cache_stale_up), or a property of the element that changes its pixels (clip box, blur; for a
     shadow also the shadow and, on a shape, its geometry) (slate_cache_stale);
   - the part of it that is on screen (plus the effect's reach) is no longer inside the part that was built (big
     groups are built clipped to the screen).
   Moves keep the images: cached elements are placed on whole pixels (slate_el_place), so a move by any amount,
   whole or fractional, is a shift of the image. Opacity, visibility and colours keep them too. An allocation
   failure is never fatal: the element is drawn without the cache (a group directly, without its blur or shadow;
   a blur whose temporary memory is missing leaves the image unblurred for that frame only), it counts as a miss,
   and the next frame tries again. */
#ifndef SLATE_CACHE_H
#define SLATE_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scene.h"
#include "slate_gfx.h"

typedef struct { slate_buf_t b; uint8_t *a; } slate_bufa_t;   /* a: A8, same stride as b.px */

/* Graphics table for slate_bufa_t destinations (every slate_buf_t passed to it must be the b of a
   slate_bufa_t). */
const slate_gfx_ops_t *slate_a8_ops(void);

typedef struct slate_cache {
    /* content: w x h RGB565 premultiplied, then w x h A8 (stride w), in one block of cbytes */
    uint16_t *px;
    uint8_t *a;
    size_t cbytes;
    slate_rect_t crel;          /* where the content image is, relative to the (whole-pixel) origin */
    bool content_ok;
    /* shadow: A8 sw x sh (stride = width of srel) in one block of sbytes */
    uint8_t *sa;
    size_t sbytes;
    slate_rect_t srel;          /* where the shadow mask is, relative to the origin (offset included) */
    bool shadow_ok;
    uint32_t frame;             /* last frame that resolved this cache */
    bool usable;                /* result of that resolution */
    bool gave_up;               /* a scroll's content image lost children's effects for lack of memory: not built
                                   again until its content changes (compose.c) */
    void (*free)(void *ctx, void *p);
    void *ctx;
} slate_cache_t;

/* e's record, created on first use (NULL when out of memory). */
slate_cache_t *slate_cache_of(slate_el_t *e);
/* Marks e's own content and/or shadow image stale. */
void slate_cache_stale(slate_el_t *e, bool content, bool shadow);
/* Marks stale every image of every ancestor of e (something inside them changed). */
void slate_cache_stale_up(slate_el_t *e);
/* Frees e's images and record. */
void slate_cache_drop(slate_el_t *e);
/* Frees the images of e and of everything under it (e.g. for a scene that is no longer shown and does not keep its
   caches, or a kept one whose room is needed: engine.h, Kept scenes); they are rebuilt when needed again. */
void slate_cache_drop_tree(slate_el_t *e);
/* Makes *slot hold at least need bytes from alloc (reusing it when it already does); NULL on failure, with the
   old block freed. */
void *slate_cache_mem(slate_cache_t *c, void **slot, size_t *have, size_t need, void *(*alloc)(void *ctx, size_t),
                      void (*free_fn)(void *ctx, void *p), void *ctx);

#endif
