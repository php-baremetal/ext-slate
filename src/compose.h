/* compose.h - Slate's band compositor: draws the dirty regions of a scene into bands and hands them over.
   Pure C, no ESP-IDF and no PHP; it never calls back into PHP. Not reentrant: one compositor (task) at a time.

   A frame (slate_compose). Nothing dirty: returns at once, emit is never called. It asks the target for its two
   band buffers (band_buf(ctx, 0) and (ctx, 1): addresses only, nothing is written yet); if either is NULL the
   frame is aborted before anything is emitted (stats.aborted = 1, the dirty set is kept for a retry).
   Otherwise, for every dirty rectangle (top to bottom by y0), band after band of target->rows rows (the last one
   may be shorter; a region narrower than the screen takes as many rows as a buffer of w * rows pixels holds):
   1. waits until the band buffer it is about to write is free (target->wait_free), alternating buffers 0 and 1
      continuously across frames (a frame starts with the buffer after the one the previous frame used last,
      which is the one the panel freed first);
   2. lists the elements that touch the band in z-order, flattening plain groups (opacity 1, no cache, shadow or
      blur: their children draw straight into the band) and taking the other groups as one item (the list of the
      whole screen is made once per frame and cut to each band);
   3. occlusion: walking the list from the top, an item whose visible part in the band (box cut by its clips)
      lies inside the opaque interior of an item above it is skipped and counted in `culled`; the background is
      skipped the same way. Opaque interior (only for elements whose e->opaque is true, cut by their clips):
      ROUNDRECT: its box shrunk inward to whole pixels and then by ceil(radius) on the two sides that have the
      corners (the wider of the two cross-shaped rectangles), and by the border on every side when the border
      colour is not opaque; CIRCLE: the square inscribed in the circle of radius r - 0.5 (minus a non-opaque
      border); CANVAS: its box. Up to 8 occluders (the largest) are kept per band;
   4. fills the background (target->background) unless covered (or copies the underlay, see below), then draws
      the items bottom to top: shapes and
      text through raster.h (opaque spans are plain fills, only edges blend), a canvas as a copy, a group:
      - with cache, shadow or blur: its cached image (cache.h) copied with copy_a times its opacity, over its
        shadow (fill_mask of the blurred alpha); without cache memory it falls back to the next two cases;
      - with opacity < 1: composed offscreen (RGB565 + A8, premultiplied) in strips taken from
        target->scratch (internal RAM), each strip copy_a'd into the band with the group's opacity; when
        scratch cannot hold a row the children are drawn straight into the band, opacity ignored, and
        stats.degraded counts it (offscreen work never blends in cache memory);
      - otherwise its children are drawn straight into the band;
   5. converts the band to the panel's byte order (high byte first, slate_swap565_row, in place: the compositor
      swaps, not the engine) and calls emit(ctx, region, y0, rows, px): px holds rows x region width pixels,
      stride = region width (contiguous), screen rows y0 .. y0 + rows - 1. The bands of one region are emitted
      in order, the first with y0 == region.y0, the last ending at region.y1: a driver can open the panel window
      on the first and close it on the last. After emit the buffer belongs to the target until it reports it
      free through wait_free; the compositor writes it again only after wait_free(ctx, i) returns.
   The dirty set is left empty.

   Underlay (slate_compose_underlay): the background and the bottom items that have not changed for a few
   frames (P: the longest run of items from the bottom, in z-order, none of which, nor a group holding them,
   changed in the last 2 frames; an item drawn through a cache or scratch, which can come out differently with
   memory, ends it) are kept as a screen-sized RGB565 image in cache memory, valid where it was written. A band
   whose rectangle is valid there copies it instead of filling the background and drawing P; another band draws
   them (nothing above culls them then), writes the result into the image and marks it valid, then draws the rest.
   The same pixels either way: a shape drawn band by band gives the pixels of one drawn whole. A change to P (or to
   the scene, the background colour, the screen size) defines P again with an empty image. Only when P holds
   something that costs more to draw than to copy (a gradient, an arc, a line, text, a border); the image
   (w x h x 2 bytes plus a bit per pair of columns per row) comes from cache_alloc and is given back when an
   element's cache needs the memory (the underlay then stays off until another scene is shown).

   Placement: elements drawn through a cache sit on whole pixels (scene.h, "Placement").

   Stats (overwritten by every call): drawn = items drawn, counted once per band or offscreen strip they are drawn
   into (children of a group composed offscreen count too); culled = items skipped by occlusion, likewise per band;
   cache_hit / cache_miss = per element and frame, when an element's cached images are used as they are / have
   to be built (or cannot be: allocation failure); area = pixels composed (sum of the bands); degraded = groups
   drawn without their opacity for lack of scratch; aborted = 1 when the frame was not composed (band_buf NULL);
   scratch_want = the most scratch the frame asked scratch_grow for (bytes, 0: none asked; see slate_target_t). */
#ifndef SLATE_COMPOSE_H
#define SLATE_COMPOSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dirty.h"
#include "scene.h"
#include "slate_gfx.h"

typedef struct { uint32_t drawn, culled, cache_hit, cache_miss, area, degraded, aborted, scratch_want; } slate_stats_t;

typedef void (*slate_emit_fn)(void *ctx, slate_rect_t region, int y0, int rows, uint16_t *px);

/* Where the bands go and where memory comes from. Mapping onto ext-display's band contract (Task 8):
   band_buf returns the two buffers obtained with buf_alloc; emit opens the region with band_begin when
   y0 == region.y0, calls band_push with a done callback that marks the buffer (px is band_buf(i) for the i
   used last) free, and band_end when y0 + rows == region.y1; wait_free blocks on that mark. */
typedef struct {
    int w, h;          /* screen size */
    int rows;          /* band height, >= 1 */
    void *ctx;         /* passed to every callback below */
    /* Address of band buffer i (0 or 1): at least w * rows pixels, 4-byte aligned, in fast internal RAM. Called
       for both at the start of every frame; NULL aborts the frame. */
    uint16_t *(*band_buf)(void *ctx, int i);
    /* Blocks until buffer i, handed to emit before, is free again. NULL: emit is synchronous. */
    void (*wait_free)(void *ctx, int i);
    /* Memory for the element caches (PSRAM on the board) and for building them; cache_alloc NULL: nothing is
       cached. Must return 4-byte aligned blocks (or NULL). */
    void *(*cache_alloc)(void *ctx, size_t bytes);
    void (*cache_free)(void *ctx, void *p);
    /* Fast scratch memory (internal RAM) for groups composed offscreen and for building caches. Required for
       group opacity and caches: a strip of a group needs (width * 3 + 3) & ~3 bytes per row, and each nesting
       level gets half of what is left, so the minimum is 2 * (3 * w + 3) bytes; recommended 3 * w * rows (one
       band's worth of strips, e.g. 43 KB for 360 x 40). Without it (NULL / too small) opacity groups are drawn
       without their opacity and caches are not built. */
    void *scratch;
    size_t scratch_bytes;
    uint16_t background;   /* RGB565 native, under everything */
    /* Optional scratch on demand (NULL: scratch and scratch_bytes are all there is). Called only while nothing
       lives in the scratch (before a group composed offscreen or a cache built that is not nested in another
       one), with the bytes that work wants, at most scratch_max: the target makes the scratch at least that big
       when it can (it may move: the old one is not used again) and returns it, its size in *got (NULL and 0:
       none). The bytes asked for are what a scratch of scratch_max bytes gives that work (the same strips), and
       at least 2^(d+1) rows of the group when d groups composed offscreen nest in it, so that a group drawn with
       its opacity from scratch_max bytes is drawn with it from what was asked for. Same pixels either way. */
    void *(*scratch_grow)(void *ctx, size_t bytes, size_t *got);
    size_t scratch_max;
} slate_target_t;

/* Composes the dirty regions of the scene under root (root itself included: its origin is its parent's origin,
   (0, 0) for a real root) and empties d. emit and every target callback are called from this function only. */
void slate_compose(slate_el_t *root, slate_dirty_t *d, const slate_target_t *t, slate_emit_fn emit, void *ctx,
                   slate_stats_t *st);

/* Turns the underlay on or off for the frames that follow (off by default; off keeps the one there is, unused). */
void slate_compose_underlay(bool on);

/* Frees the compositor's own working memory (the item list, which otherwise grows to the largest scene seen). */
void slate_compose_release(void);

#endif
