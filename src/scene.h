/* scene.h - Slate's scene graph: elements, groups, z-order, properties and bounding boxes.
   Pure C, no ESP-IDF and no PHP. Owned by the compositor task: nothing here is thread-safe.

   Tree. A Group holds children in z-order (first = bottom, last = top). Every other kind is a leaf. An element
   has at most one parent. Positions (x, y and the Line's second end point) are relative to the parent's origin;
   the parent's origin is the screen position of its own (x, y), summed up the chain (the root's x, y included).

   Geometry per kind (float, sub-pixel positions allowed, see raster.h):
     ROUNDRECT  box (x, y, w, h), radius, fill, border, border_color, shadow
     CIRCLE     centre (x, y), r, fill, border, border_color, shadow
     ARC        centre (x, y), r, from, to (degrees, 0 = 3 o'clock, clockwise), width, color, caps
     LINE       (x, y) to (x1, y1) (pixel centres), width, color (solid argb only), caps
     TEXT       (x, y) top of the line (rounded to whole pixels), text, font, color (solid argb only), align,
                max_width
     CANVAS     box (x, y, w, h) rounded to whole pixels; its own RGB565 buffer (opaque), see slate_canvas_*
     IMAGE      top-left (x, y) rounded to whole pixels; image (images.h, a reference held): drawn once the engine
                has seen it READY (slate_el_image_sync), its size the file's (0 x 0 before, or failed, or none)
     GROUP      origin (x, y); opacity, clip (to the box (0, 0, w, h) of the group, in its own coordinates; w or
                h <= 0: clip ignored), cache, blur, shadow
   Common: x, y, visible, opacity (0..1, a multiplier of the colours' alpha; a group's opacity applies to the
   group as a whole, composed offscreen).

   Bounding box (slate_el_bbox): screen rectangle that contains every pixel the element can write: its geometry
   rounded outward plus SLATE_AA_MARGIN pixel of anti-aliasing fringe on each side, joined with its shadow (offset
   and blur extent); for a group the union of its visible children (cut to its clip box), grown by its blur and
   joined with its shadow. A hidden element (or one inside a hidden group) has an empty box. The box does not
   take the clips of the ancestors into account; slate_el_dirty_rect does.

   Placement. Elements drawn through a cache (slate_el_snaps) are placed on whole pixels: their origin (x, y plus
   the parent's origin) is rounded to the nearest pixel, so a cached image is reused, not rebuilt, when they move
   by a fraction of a pixel (an animation moves them every frame). Their children keep their own sub-pixel
   offsets inside. Every other element keeps its sub-pixel position and anti-aliasing.

   Effects. Shadow (ROUNDRECT, CIRCLE, GROUP), group blur and gradient paints are accepted even when the effect
   is not built (slate_config.h), and then ignored: a shadow or blur that is not built adds nothing to the box
   and is not drawn, a gradient paint draws nothing (the PHP layer refuses them before they get here).
   Shadow and blur radii are box-blur radii r (three passes, see slate_gfx.h): about a gaussian of sigma
   sqrt(r(r+1)) ~ r + 0.5, i.e. CSS filter: blur(r + 0.5px) (a CSS box-shadow blur value is 2 sigma); the blurred
   image reaches SLATE_BLUR_EXTENT(r) = 3r pixels past the shape.

   Dirty regions. Every change (slate_el_set, slate_el_add, slate_el_remove, slate_canvas_dirty) adds to d the
   screen area that may change: the element's slate_el_dirty_rect before and after it (d NULL: nothing is
   recorded, e.g. for a scene that is not shown). A move of a ROUNDRECT or CIRCLE with a shadow that leaves it on
   the same whole pixel adds nothing (none of its pixels moves), unless it is inside a group drawn through an image.
   It also marks stale the caches it affects (cache.h).

   Size: a common header (52 bytes) plus the largest kind (ROUNDRECT / CIRCLE, 40 bytes): 92 bytes per element
   on the ESP32-S3 (120 on a 64-bit host), from the scene allocator (slate_scene_set_alloc); texts, gradients and
   canvas pixels are allocated apart, and a cache record (76 bytes) only for elements that use a cache.

   Opaque. e->opaque is true when the element covers its interior with fully opaque pixels: a ROUNDRECT or
   CIRCLE with a solid fill of alpha 255 (or a gradient whose stops are all alpha 255, when gradients are built)
   and opacity 1, or a CANVAS with opacity 1, or an IMAGE that is shown (sync), opaque (the .sli flag) and at
   opacity 1. The compositor uses it to skip what is underneath (compose.h).

   Scroll. A GROUP made a scroll (SLATE_PROP_SCROLL, a copy of a slate_scroll_t of scroll.h; the record, 40 bytes
   plus the physics, from the scene allocator, only for scrolls; the element's size does not change) is a view (0, 0, w, h) over its children, the
   content, in content coordinates: they are drawn shifted by minus its drawn position (SCROLL_X, SCROLL_Y, rounded
   to whole pixels, slate_el_scroll_px), so the origin of its children (slate_el_parent_origin) includes that shift.
   Its clip is always on (CLIP false is refused) and cuts the view even with w or h <= 0 (then nothing shows). The
   content size is CONTENT_W / CONTENT_H, or for 0 the box of the visible children from (0, 0) (their geometry, no
   shadow, rounded up), computed again whenever the children's extent, the content size or the view changes; the
   physics' limits follow (slate_scroll_limits) before any use of them (slate_el_scroll and the functions below).
   Its box is its view (0, 0, w, h) (grown by its blur), whatever its content and position: that is what it counts
   for in its parent's box and in an outer scroll's content size. A change of the drawn position marks only the view
   (on the same whole pixel: nothing), and keeps the content image of a cached scroll (cache.h); a change under it
   marks as for any group; a change of its content size marks only its bars (nothing without them). Bars (indicators): SLATE_SCROLL_BAR px thick, rounded,
   SLATE_SCROLL_BAR_GAP px from the view's right (vertical bar) and bottom (horizontal bar) edges, only on the axes
   the scroll allows that have something to scroll (content > view); along its axis a bar runs on a track from
   SLATE_SCROLL_BAR_GAP to view - SLATE_SCROLL_BAR_GAP (minus the other bar's corner, SLATE_SCROLL_BAR +
   SLATE_SCROLL_BAR_GAP, when both show), its length view^2 / content, at least SLATE_SCROLL_BAR_MIN and at most the
   track, at an offset (track - length) * position / max rounded; past an edge it is shorter by the overscroll (down to
   SLATE_SCROLL_BAR px) and sits against the end of its track. Drawn over the content in SLATE_SCROLL_BAR_ARGB times
   INDICATOR_A (written by the engine as they fade): 0 draws none, and a change of it marks only their boxes.

   Images. An IMAGE element holds a reference to its image (taken by slate_el_set, dropped when the image changes
   and by slate_el_free) and the image's pixels once seen READY (u.image.sli: what the compositor draws). The
   image becomes READY in the loader task, at any moment: the element shows it only from slate_el_image_sync on
   (the engine calls it when images finished loading), which marks its box dirty, so a frame never shows an image
   that its dirty set does not cover. slate_el_set of an image that is READY already shows it at once. */
#ifndef SLATE_SCENE_H
#define SLATE_SCENE_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dirty.h"
#include "raster.h"
#include "scroll.h"
#include "slate_gfx.h"
#include "sli.h"

#define SLATE_AA_MARGIN 1
#define SLATE_BLUR_EXTENT(r) (3 * (r))

/* Scroll bars (see "Scroll" above): thickness, gap to the view's edges, shortest length (px), colour (straight argb,
   times the indicator alpha and the scroll's opacity). */
#define SLATE_SCROLL_BAR 3
#define SLATE_SCROLL_BAR_GAP 2
#define SLATE_SCROLL_BAR_MIN 16
#define SLATE_SCROLL_BAR_ARGB 0x99808080u

typedef enum {
    SLATE_EL_GROUP = 1,
    SLATE_EL_ROUNDRECT,
    SLATE_EL_CIRCLE,
    SLATE_EL_ARC,
    SLATE_EL_LINE,
    SLATE_EL_TEXT,
    SLATE_EL_CANVAS,
    SLATE_EL_IMAGE,
} slate_el_kind_t;

/* Properties, with the value type slate_el_set expects (a value of another type is refused). */
typedef enum {
    SLATE_PROP_X,             /* NUM */
    SLATE_PROP_Y,             /* NUM */
    SLATE_PROP_W,             /* NUM: ROUNDRECT, CANVAS (whole pixels), GROUP (clip box) */
    SLATE_PROP_H,             /* NUM */
    SLATE_PROP_VISIBLE,       /* BOOL */
    SLATE_PROP_OPACITY,       /* NUM, clamped to 0..1 */
    SLATE_PROP_FILL,          /* PAINT: ROUNDRECT, CIRCLE (the gradient is copied) */
    SLATE_PROP_BORDER,        /* NUM: border width, ROUNDRECT, CIRCLE */
    SLATE_PROP_BORDER_COLOR,  /* COLOR (a PAINT is accepted, its argb used) */
    SLATE_PROP_RADIUS,        /* NUM: ROUNDRECT corner radius */
    SLATE_PROP_R,             /* NUM: CIRCLE, ARC radius */
    SLATE_PROP_FROM,          /* NUM: ARC, degrees */
    SLATE_PROP_TO,            /* NUM: ARC, degrees */
    SLATE_PROP_WIDTH,         /* NUM: ARC, LINE stroke width */
    SLATE_PROP_COLOR,         /* PAINT: ARC (gradient allowed), LINE and TEXT (argb only) */
    SLATE_PROP_CAPS,          /* ENUM slate_caps_t: ARC, LINE */
    SLATE_PROP_X1,            /* NUM: LINE second end point */
    SLATE_PROP_Y1,            /* NUM */
    SLATE_PROP_TEXT,          /* TEXT: UTF-8, copied (NULL: empty) */
    SLATE_PROP_FONT,          /* FONT: an entry of display_fonts[], kept by pointer */
    SLATE_PROP_ALIGN,         /* ENUM slate_align_t: TEXT */
    SLATE_PROP_MAX_WIDTH,     /* NUM: TEXT, whole pixels, 0 = no limit */
    SLATE_PROP_CLIP,          /* BOOL: GROUP */
    SLATE_PROP_CACHE,         /* BOOL: GROUP */
    SLATE_PROP_BLUR,          /* NUM: GROUP blur radius, whole pixels, 0..SLATE_BLUR_RADIUS_MAX */
    SLATE_PROP_SHADOW,        /* SHADOW: ROUNDRECT, CIRCLE, GROUP */
    SLATE_PROP_IMAGE,         /* IMAGE: IMAGE (NULL: none); a reference is taken */
    SLATE_PROP_SCROLL,        /* SCROLL: GROUP; makes it a scroll with a copy of the given physics state, NULL: a
                                 plain group again (see "Scroll" above) */
    SLATE_PROP_SCROLL_X,      /* NUM: scroll only, the drawn position (overscroll included) */
    SLATE_PROP_SCROLL_Y,      /* NUM */
    SLATE_PROP_CONTENT_W,     /* NUM: scroll only, whole pixels 0..32767, 0 = from the children */
    SLATE_PROP_CONTENT_H,     /* NUM */
    SLATE_PROP_INDICATOR_A,   /* NUM: scroll only, alpha of the scroll bars, clamped to 0..1 */
} slate_prop_t;

typedef enum {
    SLATE_VAL_NUM,
    SLATE_VAL_BOOL,
    SLATE_VAL_ENUM,
    SLATE_VAL_COLOR,
    SLATE_VAL_PAINT,
    SLATE_VAL_TEXT,
    SLATE_VAL_FONT,
    SLATE_VAL_SHADOW,
    SLATE_VAL_IMAGE,
    SLATE_VAL_SCROLL,
} slate_val_type_t;

/* A drop shadow: offset (dx, dy) in pixels, blur radius (box radius, 0..SLATE_BLUR_RADIUS_MAX) and straight
   0xAARRGGBB colour; alpha 0 = no shadow. Drawn under the element: for a ROUNDRECT or CIRCLE from the shape's
   coverage (its fill alpha does not matter, like a CSS box-shadow), for a GROUP from the alpha of its composed
   content (like a CSS drop-shadow). */
typedef struct { int16_t dx, dy, blur; uint32_t argb; } slate_shadow_t;

struct slate_image;   /* images.h */

typedef struct {
    slate_val_type_t type;
    union {
        float num;
        bool b;
        int e;                                     /* slate_caps_t, slate_align_t */
        uint32_t argb;                             /* COLOR */
        slate_paint_t paint;                       /* PAINT: the gradient and its stops are copied */
        const char *text;
        const struct display_font_entry *font;
        slate_shadow_t shadow;
        struct slate_image *image;         /* IMAGE: the receiver takes its own reference */
        const slate_scroll_t *scroll;      /* SCROLL: copied by the receiver; NULL: none */
    } v;
} slate_value_t;

static inline slate_value_t slate_v_num(float f) { slate_value_t v = { SLATE_VAL_NUM, { .num = f } }; return v; }
static inline slate_value_t slate_v_bool(bool b) { slate_value_t v = { SLATE_VAL_BOOL, { .b = b } }; return v; }
static inline slate_value_t slate_v_enum(int e) { slate_value_t v = { SLATE_VAL_ENUM, { .e = e } }; return v; }
static inline slate_value_t slate_v_color(uint32_t c) { slate_value_t v = { SLATE_VAL_COLOR, { .argb = c } }; return v; }
static inline slate_value_t slate_v_paint(slate_paint_t p) { slate_value_t v = { SLATE_VAL_PAINT, { .paint = p } }; return v; }
static inline slate_value_t slate_v_text(const char *s) { slate_value_t v = { SLATE_VAL_TEXT, { .text = s } }; return v; }
static inline slate_value_t slate_v_font(const struct display_font_entry *f)
{
    slate_value_t v = { SLATE_VAL_FONT, { .font = f } };
    return v;
}
static inline slate_value_t slate_v_image(struct slate_image *im)
{
    slate_value_t v = { SLATE_VAL_IMAGE, { .image = im } };
    return v;
}
static inline slate_value_t slate_v_scroll(const slate_scroll_t *s)
{
    slate_value_t v = { SLATE_VAL_SCROLL, { .scroll = s } };
    return v;
}
/* dx, dy clamped to +-32767, blur to 0..SLATE_BLUR_RADIUS_MAX */
static inline slate_value_t slate_v_shadow(int dx, int dy, int blur, uint32_t argb)
{
    dx = dx < -32767 ? -32767 : dx > 32767 ? 32767 : dx;
    dy = dy < -32767 ? -32767 : dy > 32767 ? 32767 : dy;
    blur = blur < 0 ? 0 : blur > SLATE_BLUR_RADIUS_MAX ? SLATE_BLUR_RADIUS_MAX : blur;
    slate_value_t v = { SLATE_VAL_SHADOW, { .shadow = { (int16_t)dx, (int16_t)dy, (int16_t)blur, argb } } };
    return v;
}

struct slate_cache;   /* cache.h */
typedef struct slate_el slate_el_t;

/* Per-kind data (the union of struct slate_el). */
typedef struct {          /* ROUNDRECT: box w x h, corner radius r; CIRCLE: radius r (w, h unused) */
    float w, h, r, border;
    slate_paint_t fill;   /* gradient owned */
    uint32_t border_argb;
    slate_shadow_t shadow;
} slate_shape_t;
/* A scroll's state (GROUP with SLATE_PROP_SCROLL), from the scene allocator. */
typedef struct {
    slate_scroll_t phys;        /* the physics, stepped by the engine; its limits kept up to date (slate_el_scroll) */
    float sx, sy;               /* drawn position (SCROLL_X, SCROLL_Y); the children are drawn at -round() of it */
    int16_t cw, ch;             /* explicit content size, 0 = from the children */
    uint8_t ind;                /* scroll bars' alpha, 0..255 */
    bool stale;                 /* the children's extent and the physics' limits are to compute again */
    float ux0, uy0, ux1, uy1;   /* derived: the union of the visible children's boxes (shadows included), content
                                   coordinates; ux0 >= ux1: none */
    float aw, ah;               /* derived: the automatic content size (the children's box from (0, 0)) */
} slate_scroll_el_t;

typedef struct {
    slate_el_t *first, *last;   /* children, bottom .. top */
    float w, h;                 /* clip box */
    slate_shadow_t shadow;
    int16_t blur;
    slate_scroll_el_t *scroll;  /* NULL: not a scroll */
} slate_group_t;
typedef struct { float r, from, to, width; slate_paint_t color; uint8_t caps; } slate_arc_t;
typedef struct { float x1, y1, width; uint32_t argb; uint8_t caps; } slate_line_t;
typedef struct {
    char *text;                              /* owned, NULL = empty */
    const struct display_font_entry *font;
    uint32_t argb;
    int16_t max_width;
    uint8_t align;
    int16_t bx0, by0, bx1, by1;              /* slate_text_bbox at (0, 0) */
} slate_text_t;
typedef struct { uint16_t *px; int16_t w, h; } slate_canvas_t;   /* w x h RGB565 native, stride w; NULL: empty */
typedef struct {
    struct slate_image *im;   /* a reference held; NULL: none */
    const slate_sli_t *sli;   /* im's pixels once seen READY (slate_el_image_sync); NULL: nothing drawn */
} slate_image_el_t;

/* The fields are read by the compositor; change them only through the functions below. The struct is a common
   header plus the data of its kind (u.g, u.shape, u.arc, u.line, u.text, u.canvas, u.image). */
struct slate_el {
    slate_el_t *parent, *prev, *next;
    struct slate_cache *cache;           /* NULL until the compositor needs one */
    float x, y;                          /* relative to the parent (CIRCLE, ARC: centre; LINE: first end) */
    float cx0, cy0, cx1, cy1;            /* derived: content extent in the parent's coordinates (no shadow, no
                                            AA margin, unsnapped); cx0 >= cx1: nothing */
    uint32_t mod_self;                   /* slate_scene_clock() when e itself last changed (a property, a child
                                            added or removed) */
    uint32_t mod_sub;                    /* the same for e or anything under it */
    uint8_t kind;                        /* slate_el_kind_t */
    uint8_t alpha;                       /* opacity * 255, rounded */
    bool visible : 1, clip : 1, cached : 1, opaque : 1, ext_ok : 1;   /* clip, cached: GROUP properties */
    bool under : 1;                      /* the compositor's: e is drawn in its underlay (compose.c) */
    bool anim : 1;                       /* the engine's: e may have tweens or ease settings (engine.c); a hint,
                                            nothing here reads it */
    union {
        slate_group_t g;
        slate_shape_t shape;
        slate_arc_t arc;
        slate_line_t line;
        slate_text_t text;
        slate_canvas_t canvas;
        slate_image_el_t image;
    } u;
};

/* Change clock. Every change of an element (slate_el_set, slate_el_add, slate_el_remove, slate_canvas_dirty, and the
   creation) stamps it with the current value: mod_self on the element changed (on a group, also when a child is
   added or removed), mod_sub on it and on every ancestor. The compositor advances the clock once per frame
   (slate_scene_tick) and reads the stamps to find what has not changed for a while. */
uint32_t slate_scene_clock(void);
uint32_t slate_scene_tick(void);

/* Memory for elements and everything they own (texts, gradients, canvas pixels, cache records); the default is
   malloc / free. alloc need not zero. Set it before the first element is created (and never change it while
   elements live): an element is freed with the function that is current then. */
void slate_scene_set_alloc(void *(*alloc)(size_t bytes), void (*free_fn)(void *p));
/* Zeroed memory from that allocator (NULL when out of memory), and its release (NULL allowed). */
void *slate_scene_mem(size_t bytes);
void slate_scene_mem_free(void *p);

/* A new element with the defaults of the PHP API (visible, opacity 1, everything else 0 / none, caps butt,
   align left; a Text's colour opaque white). NULL when out of memory. */
slate_el_t *slate_el_new(slate_el_kind_t kind);
/* Detaches e (without recording anything dirty: remove it first when it is shown) and frees it, its children
   and their caches. */
void slate_el_free(slate_el_t *e);
/* Appends child on top of group's children. false (nothing done) when group is not a GROUP, child already has
   a parent, or child is group or one of its ancestors. */
bool slate_el_add(slate_el_t *group, slate_el_t *child, slate_dirty_t *d, int sw, int sh);
/* Detaches child from its parent (no-op without one); the child stays alive. */
void slate_el_remove(slate_el_t *child, slate_dirty_t *d, int sw, int sh);
/* Sets a property. false (nothing changed) when the kind has no such property or the value has the wrong type,
   or out of memory for a gradient, text or canvas (the old value stays). */
bool slate_el_set(slate_el_t *e, slate_prop_t prop, const slate_value_t *v, slate_dirty_t *d, int sw, int sh);
/* slate_el_set of X to x, then of Y to y (the results in *ok_x, *ok_y), as one move: the screen only ever shows the
   element before and after both, so d gets those two boxes and not the one in between. */
void slate_el_move(slate_el_t *e, const slate_value_t *x, const slate_value_t *y, slate_dirty_t *d, int sw, int sh,
                   bool *ok_x, bool *ok_y);

/* The current value of an animatable property of e (tween.h), for the engine's tweens: NUM for X, Y, OPACITY (the
   stored alpha / 255), W and H (ROUNDRECT, GROUP), BLUR (GROUP), RADIUS (ROUNDRECT), BORDER (ROUNDRECT, CIRCLE), R
   (CIRCLE, ARC), FROM and TO (ARC), WIDTH (ARC, LINE), X1 and Y1 (LINE), SCROLL_X and SCROLL_Y (a scroll: only the
   engine animates them, scrollTo); COLOR for FILL (ROUNDRECT, CIRCLE),
   BORDER_COLOR (ROUNDRECT, CIRCLE) and COLOR (ARC, LINE, TEXT). false when e's kind does not animate prop (X, Y and
   OPACITY are every kind's; a CANVAS's W and H are not animated, nor any other property) or the paint is a gradient
   now. */
bool slate_el_anim_get(const slate_el_t *e, slate_prop_t prop, slate_value_t *out);

/* Screen origin of e's parent (0, 0 for a root). */
void slate_el_parent_origin(const slate_el_t *e, float *ox, float *oy);
/* True for the elements drawn through a cache (a GROUP with cache, a built shadow or a built blur; a ROUNDRECT or
   CIRCLE with a built shadow): they are placed on whole pixels (see "Placement" above). */
bool slate_el_snaps(const slate_el_t *e);
/* Adjusts the parent origin (*pox, *poy) so that e's own origin lands on whole pixels when e snaps; no-op
   otherwise, and idempotent. Every screen computation of e (box, clip, drawing) goes through it. */
void slate_el_place(const slate_el_t *e, float *pox, float *poy);
/* Screen bounding box, shadow and AA included (see above). */
slate_rect_t slate_el_bbox(const slate_el_t *e);
/* The screen area a change of e can affect: its box, cut by the clip of every ancestor; when an ancestor has a
   shadow or a blur (built) or a cache the area becomes that ancestor's whole box (its effects follow its content,
   and its image is built again whole). Empty when e or an ancestor is hidden. */
slate_rect_t slate_el_dirty_rect(const slate_el_t *e);
/* Extent helpers for the compositor: e's extent (or content extent: without its shadow) on screen given its
   parent's origin, AA margin included. */
slate_rect_t slate_el_screen_ext(slate_el_t *e, float pox, float poy, bool content);
/* Screen clip box of group g (its (0, 0, w, h), rounded to whole pixels) given its parent's origin. */
slate_rect_t slate_el_clip_rect(const slate_el_t *g, float pox, float poy);
/* True when g is a GROUP whose clip applies (clip set, w and h > 0). */
bool slate_el_clips(const slate_el_t *g);
/* e's shadow as stored (NULL for kinds without one), and the effects that are built and set: blur radius (0 when
   none), shadow present. */
const slate_shadow_t *slate_el_shadow(const slate_el_t *e);
int slate_el_blur(const slate_el_t *e);
bool slate_el_has_shadow(const slate_el_t *e);

/* Canvas: its pixels (cw x ch, stride cw, RGB565 native, opaque, cleared to 0 on every resize), written by the
   owner; after writing call slate_canvas_dirty (r in the canvas's own pixels, clipped to it; NULL = whole). */
uint16_t *slate_canvas_px(slate_el_t *e, int *w, int *h);
void slate_canvas_dirty(slate_el_t *e, const slate_rect_t *r, slate_dirty_t *d, int sw, int sh);

/* Scroll (see "Scroll" above). The physics state of scroll e with its limits up to date (view, content), NULL when
   e is not a scroll. */
slate_scroll_t *slate_el_scroll(slate_el_t *e);
/* The content size of scroll e: the explicit one (CONTENT_W/H) or, for 0, the children's box from (0, 0) (0 x 0 when
   e is not a scroll). */
void slate_el_scroll_extent(slate_el_t *e, float *cw, float *ch);
/* The area the children of scroll e can draw, in content coordinates (whole pixels, AA margin included): the area of
   its content image when cached; on an axis the scroll cannot move, cut to the view's span at the position written
   there. Empty when it has none or is not a scroll. */
slate_rect_t slate_el_scroll_area(slate_el_t *e);
/* The scroll bars of scroll e at its drawn position, whatever their alpha: up to two boxes in e's own coordinates
   (the view's top-left at (0, 0)), the vertical one first; returns how many (0 when e is not a scroll). */
int slate_el_scroll_bars(slate_el_t *e, slate_rect_t bars[2]);
/* The drawn position of e rounded to whole pixels (0, 0 when e is not a scroll): e's children are drawn shifted by
   minus that. */
static inline void slate_el_scroll_px(const slate_el_t *e, int *dx, int *dy)
{
    const slate_scroll_el_t *s = e->kind == SLATE_EL_GROUP ? e->u.g.scroll : NULL;
    *dx = s ? (int)floorf(s->sx + 0.5f) : 0;
    *dy = s ? (int)floorf(s->sy + 0.5f) : 0;
}

/* Image: when e is an IMAGE whose image is READY but not shown yet, shows it (its box and the opaque flag change)
   and adds its box to d, as any change does; true then. false (nothing done) otherwise. */
bool slate_el_image_sync(slate_el_t *e, slate_dirty_t *d, int sw, int sh);

#endif
