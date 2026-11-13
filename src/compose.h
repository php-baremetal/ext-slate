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
      border); CANVAS: its box; IMAGE: its box (opaque only when its .sli says that every pixel is, which
      slate_sli_open checked). Up to 8 occluders (the largest) are kept per band;
   4. fills the background (target->background) unless covered (or copies the underlay, see below), then draws
      the items bottom to top: shapes and
      text through raster.h (opaque spans are plain fills, only edges blend), a canvas as a copy, an image with
      slate_sli_draw_ops (only its rows in the band and its runs in the area drawn: fills and copies, copy_a for
      the semi-transparent runs and for every run below opacity 1; nothing until it is shown, scene.h), a group:
      - with cache, shadow or blur: its cached image (cache.h) copied with copy_a times its opacity, over its
        shadow (fill_mask of the blurred alpha); without cache memory it falls back to the next two cases;
      - with opacity < 1: composed offscreen (RGB565 + A8, premultiplied) in strips taken from
        target->scratch (internal RAM), each strip copy_a'd into the band with the group's opacity; when
        scratch cannot hold a row the children are drawn straight into the band, opacity ignored, and
        stats.degraded counts it (offscreen work never blends in cache memory);
      - otherwise its children are drawn straight into the band;
      a scroll (scene.h, "Scroll") is drawn as a group clipped to its view, its children shifted by its drawn
      position (those outside the view are not listed, as with any clip); with cache (and no shadow or blur) its
      content image (cache.h) is copied at that position, built once (the caches of the children built inside it
      are cut to that image, not to the screen, and dropped once it is built); when cache memory or the scratch
      refuses it, or a child's shadow or blur gets no memory during the build (the image is then freed and not
      built again until the content changes), the scroll is composed directly as without cache and
      stats.scroll_nocache counts it. Its bars go over its content: for a
      scroll drawn straight into the band, an item of their own right after its children; else drawn after the
      group's content with its opacity;
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
   something that costs more to draw than to copy (a gradient, an arc, a line, text, a border, an image that
   blends: with alpha or below opacity 1); the image
   (w x h x 2 bytes plus a bit per pair of columns per row) comes from cache_alloc and is given back when an
   element's cache needs the memory (the underlay then stays off until another scene is shown).

   Placement: elements drawn through a cache sit on whole pixels (scene.h, "Placement").

   Transitions (slate_pair_*): two scene roots composed together, band by band, in the same two bands, without a
   screen-sized buffer more. slate_pair_begin puts them, each in a layer of its own (an internal plain GROUP), under
   the root the frames compose (the screen): the bottom layer holds the scene underneath, the top layer the scene on
   top over a curtain, an opaque screen-sized ROUNDRECT of that scene's background colour (its page; the curtain is an
   occluder, so what it covers of the bottom scene is skipped). The target's background must then be the bottom
   scene's (under_bg). slate_pair_at moves the layers by whole pixels (x or y: the eased progress v times the screen's
   width or height, rounded to the nearest pixel, halves up; v may leave 0..1 with an ease that overshoots) or, for a
   fade, sets the top layer's opacity to v (clamped to 0..1): then the top scene and its curtain are composed
   offscreen like any group with opacity (strips from the scratch, a band's worth at most). Directions are those of
   the movement (s = -1 for LEFT and UP, +1 for RIGHT and DOWN; d = the offset in pixels):
     FADE     bottom: from, top: to (opacity v)
     PUSH     bottom: from at s * d, top: to at s * (d - span): to enters from the side opposite the direction and
              pushes from out
     COVER    bottom: from, still; top: to at s * (d - span), sliding over it
     UNCOVER  bottom: to, still; top: from at s * d, sliding away and uncovering it
   Each layer clips to its page, the box (0, 0, w, h) at its offset: what a scene has off its page (a drawer parked
   at a negative x, a panel past an edge, a shadow or blur past an edge) never shows over the other scene, as it
   never shows on the screen when that scene is shown alone. While a pair is active the caches of its scenes are
   built for the page of their scene, not for the screen (cache.h: "big groups are built clipped to the screen"), so
   a cached group that slides onto the screen is built once, as it will be when shown alone, and only moved
   afterwards. An ease that overshoots (OutBack, OutElastic) moves the layers past their places: the strip a slide
   leaves uncovered then shows the target's background, the bottom scene's.
   The roots keep their own x, y (the layer's offset adds to them); group caches move with them (whole pixels: a
   shift of the image), and elements may change, be animated, added or removed meanwhile as usual. The caller marks
   the whole screen dirty at every step that moved the layers and, as the underlay's items move, turns the underlay off (and gives its
   memory back) for the transition. slate_pair_end takes the layers away and puts to back under the root, on top of
   its children (from is left without a parent). Scene functions only: nothing here draws, and the dirty set is not
   touched.

   Stats (overwritten by every call): drawn = items drawn, counted once per band or offscreen strip they are drawn
   into (children of a group composed offscreen count too); culled = items skipped by occlusion, likewise per band;
   cache_hit / cache_miss = per element and frame, when an element's cached images are used as they are / have
   to be built (or cannot be: allocation failure); area = pixels composed (sum of the bands); degraded = groups
   drawn without their opacity for lack of scratch; aborted = 1 when the frame was not composed (band_buf NULL);
   scratch_want = the most scratch the frame asked scratch_grow for (bytes, 0: none asked; see slate_target_t);
   scroll_nocache = cached scrolls composed directly because their content image could not be built (no cache memory,
   or no scratch to build it; per element and frame, also counted in cache_miss). */
#ifndef SLATE_COMPOSE_H
#define SLATE_COMPOSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dirty.h"
#include "scene.h"
#include "slate_gfx.h"

typedef struct {
    uint32_t drawn, culled, cache_hit, cache_miss, area, degraded, aborted, scratch_want, scroll_nocache;
} slate_stats_t;

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
       level gets half of what is left (less when that would leave a level nested deeper without a row: then each
       level leaves a row for each one under it), so the minimum is 2 * (3 * w + 3) bytes; recommended 3 * w * rows (one
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

/* Gives the underlay's memory back now (cache_free); a later frame with the underlay on defines it again. */
void slate_compose_underlay_free(void);

/* Frees the compositor's own working memory (the item list, which otherwise grows to the largest scene seen). */
void slate_compose_release(void);

/* Transitions (see above). Kinds and directions in the order of the PHP API (enum Direction). */
typedef enum {
    SLATE_TRANS_NONE,
    SLATE_TRANS_FADE,
    SLATE_TRANS_PUSH,
    SLATE_TRANS_COVER,
    SLATE_TRANS_UNCOVER,
} slate_trans_kind_t;
#define SLATE_TRANS_COUNT 5
typedef enum { SLATE_DIR_LEFT, SLATE_DIR_RIGHT, SLATE_DIR_UP, SLATE_DIR_DOWN } slate_dir_t;
#define SLATE_DIR_COUNT 4

typedef struct {
    slate_el_t *parent, *from, *to;
    slate_el_t *lay[2];      /* bottom, top: internal groups, children of parent */
    slate_el_t *curtain;     /* the top scene's background, at the bottom of the top layer */
    uint8_t kind, dir;
    int w, h;
    uint32_t under_bg;       /* the bottom scene's background (0xAARRGGBB, alpha ignored): the target's background */
} slate_pair_t;

/* Starts a transition of kind (not NONE) and dir from from (a child of parent, or NULL: only its background) to
   to (a GROUP without a parent), at v = 0. from_bg, to_bg: the scenes' backgrounds (argb, alpha ignored); w, h: the
   screen. false when out of memory (nothing changed). */
bool slate_pair_begin(slate_pair_t *p, slate_el_t *parent, slate_el_t *from, slate_el_t *to, slate_trans_kind_t kind,
                      slate_dir_t dir, uint32_t from_bg, uint32_t to_bg, int w, int h);
/* The layers at eased progress v (0: the start, 1: the end); true when that moved them (or changed the opacity). */
bool slate_pair_at(slate_pair_t *p, float v);
/* Ends it: from (when still in its layer) is detached, to goes back under parent, on top; the layers are freed. */
void slate_pair_end(slate_pair_t *p);

#endif
