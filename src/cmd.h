/* cmd.h - Slate's command buffer: how the PHP task changes the scene the compositor task owns.
   Pure C, no ESP-IDF and no PHP (locks and memory through the events HAL of ext-events).

   The PHP task never touches the scene: it appends commands (slate_cmd_push) to a pending batch and, when it
   returns to the event loop (or at show()), hands the whole batch over (slate_cmd_deliver). The compositor task
   takes the delivered batches one at a time, oldest first (slate_cmd_take), applies every command of a batch
   before it composes a frame, and gives the batch back (slate_cmd_release). A command pushed while the engine
   is taking a batch goes into the pending batch, delivered later: a batch is never seen half written.

   Elements are named by ids the PHP task assigns (uint32, 0 = none); the engine maps them to its elements.
   Everything a command points to is copied into the batch at push (texts, the gradient and its stops, the scene
   name, canvas pixels, the spec of a TWEEN or EASE, the data of a SCROLL, the physics state of a SET of
   SLATE_VAL_SCROLL): the caller's memory may go as soon as push returns. Fonts are kept by pointer (entries
   of display_fonts[], which live as long as the firmware); an image (SET of SLATE_VAL_IMAGE) and a SCROLL's pub by
   a reference taken at push and given back when the batch is released or freed (images.h, slate_scroll_pub_t).

   Threading: slate_cmdbuf_new, slate_cmdbuf_free, slate_cmd_push, slate_cmd_deliver and slate_cmd_pending belong
   to one task (PHP's); slate_cmd_take and slate_cmd_release to another (the compositor's). The two sides only
   meet in deliver / take / release, under a short lock. Memory: external RAM (PSRAM on the board). */
#ifndef SLATE_CMD_H
#define SLATE_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compose.h"
#include "scene.h"
#include "tween.h"

typedef enum {
    SLATE_CMD_NEW = 1,   /* id: a new element of kind el (the defaults of slate_el_new) */
    SLATE_CMD_SET,       /* id: property prop = v (slate_el_set) */
    SLATE_CMD_ADD,       /* id (child) on top of the children of parent (a group) */
    SLATE_CMD_REMOVE,    /* id: detached from its parent; it stays alive */
    SLATE_CMD_FREE,      /* id: destroyed (a group's children are detached first and stay alive); the id is free */
    SLATE_CMD_SHOW,      /* id: the root group of the scene to show (0: none); data: its name (for messages, may be
                            NULL); v: the background, a COLOR (another type: black); tr: the transition (kind NONE,
                            all zeros: at once; see engine.h, Transitions); keep: the scene shown until now keeps its
                            caches when it is left (false: they go, as before; see engine.h, Kept scenes) */
    SLATE_CMD_PIXELS,    /* id: a canvas; r: a rectangle in its own pixels (cut to the canvas when applied);
                            data: the pixels of r, RGB565 native, row after row (stride r.x1 - r.x0), copied into
                            the canvas and marked dirty. data is required, 0 <= x0 < x1 <= 32767 (likewise y) and
                            the pixels at most SLATE_CMD_PIXELS_MAX bytes: the PHP layer sends a canvas's own
                            rectangle (or the changed part of it) */
    SLATE_CMD_TWEEN,     /* id: the element; data: a slate_tween_spec_t (required): a tween of its properties from
                            their values now to the spec's (engine.h, Tweens) */
    SLATE_CMD_TWEEN_STOP,/* id: a tween's own id (slate_tween_spec_t::tween, not an element); v: BOOL to_end. Stops it
                            and the tweens chained after it, without a done; to_end: they write their final values
                            first. A tween that is gone (done, stopped) is not an error */
    SLATE_CMD_EASE,      /* id: the element; data: a slate_ease_spec_t (required): from now on a SET of a property in
                            its mask moves there in a tween (mask 0: none, a SET jumps again) */
    SLATE_CMD_SCROLL,    /* id: a GROUP; data: a slate_scroll_cmd_t (required): a touch of a scroll, a jump, or its
                            set-up (engine.h, Scrolls) */
} slate_cmd_kind_t;

#define SLATE_TWEEN_PROPS_MAX 12   /* properties of one tween (a ROUNDRECT animates 9) */

/* The data of a TWEEN. Pushed: the header and the first n properties are copied. */
typedef struct {
    uint32_t tween;        /* its id (not 0; the PHP task's, unique among the tweens alive): slate_engine_tween_done
                              gives it back when the tween ends by itself, TWEEN_STOP and `after` name it */
    uint32_t after;        /* the tween it starts after (its exact end), the values that one left as `from`; 0, or a
                              tween that is gone: it starts at once */
    int32_t ms;            /* one run, >= 1 */
    int32_t delay_ms;      /* before the first run, >= 0 */
    int32_t repeat;        /* runs after the first; -1: always (never done) */
    uint8_t ease;          /* slate_ease_t */
    bool yoyo;             /* the odd runs go back */
    uint8_t n;             /* properties, 1..SLATE_TWEEN_PROPS_MAX */
    slate_tween_prop_t props[SLATE_TWEEN_PROPS_MAX];   /* prop, type (NUM or COLOR, as slate_el_anim_get gives it)
                                                          and `to`; `from` is ignored (the element's value) */
} slate_tween_spec_t;

/* The data of an EASE. */
typedef struct {
    uint32_t mask;         /* bit (1u << slate_prop_t) of each eased property; 0: none */
    int32_t ms;            /* >= 1 (when mask is not 0) */
    uint8_t ease;          /* slate_ease_t */
} slate_ease_spec_t;

/* Where the engine publishes a scroll's position for the PHP task (engine.h, Scrolls): one small block in internal
   RAM (atomics), counted references (the PHP record holds one, every command naming it one, the engine one while it
   publishes into it), freed by the last slate_scroll_pub_put. Written by the engine task, read by any task without a
   lock. */
typedef struct slate_scroll_pub {
    uint32_t pos;          /* atomic: the drawn position rounded to whole pixels, (uint16_t)x | (uint16_t)y << 16,
                              each cut to int16 */
    uint32_t seq;          /* atomic: the seq of the last JUMP or CONFIG applied */
    int moving;            /* atomic: 1 while the content moves (a drag, an inertia, a snap, a spring, a scrollTo) */
    int refs;              /* atomic */
} slate_scroll_pub_t;

/* A new block (one reference, position 0, 0, not moving, seq 0); NULL when out of memory. */
slate_scroll_pub_t *slate_scroll_pub_new(void);
/* Takes / drops a reference (NULL allowed); the last drop frees it. */
void slate_scroll_pub_ref(slate_scroll_pub_t *p);
void slate_scroll_pub_put(slate_scroll_pub_t *p);
/* The position last published (whole pixels). */
void slate_scroll_pub_pos(const slate_scroll_pub_t *p, int *x, int *y);
/* true while the content moves (as last published). */
bool slate_scroll_pub_moving(const slate_scroll_pub_t *p);
/* The seq of the last JUMP or CONFIG the engine applied (0: none yet): the position read is the one after that
   command once this equals its seq. Read the seq BEFORE the position (slate_scroll_pub_pos): the engine stores the
   position first and the seq after it, so a seq seen guarantees the position read next is at least that command's;
   read the other way round, an old position may come with the new seq. */
uint32_t slate_scroll_pub_seq(const slate_scroll_pub_t *p);

/* What a SCROLL command does (slate_scroll_cmd_t::op). */
typedef enum {
    SLATE_SCROLL_OP_PRESS,     /* a finger down at (x, y) at t_us: stops any motion (a scrollTo like Tween::stop()) */
    SLATE_SCROLL_OP_DRAG,      /* a point of the drag (after the 8 px threshold: the PHP layer's) at t_us */
    SLATE_SCROLL_OP_RELEASE,   /* the finger up at t_us: inertia, snap or spring */
    SLATE_SCROLL_OP_JUMP,      /* the position (x, y), clamped to the limits; stops every motion, no end */
    SLATE_SCROLL_OP_CONFIG,    /* makes the group a scroll (or changes its set-up): axis, bounce, snap_x, snap_y,
                                  snap_max, indicators, pub */
    SLATE_SCROLL_OP_CANCEL,    /* the touch is lost (its Released will not come) at t_us: a release without velocity
                                  (slate_scroll_cancel: a spring, a snap or rest; a drag still ends) */
} slate_scroll_op_t;
#define SLATE_SCROLL_OP_COUNT 6

/* The data of a SCROLL. */
typedef struct {
    uint8_t op;            /* slate_scroll_op_t */
    uint8_t axis;          /* CONFIG: slate_scroll_axis_t (scroll.h) */
    bool bounce;           /* CONFIG */
    bool indicators;       /* CONFIG: the bars show while it moves and fade after */
    int32_t snap_x, snap_y;   /* CONFIG: px, 0 = none */
    int32_t snap_max;      /* CONFIG: snap steps a release may travel (slate_scroll_snap_max), 0 = no limit */
    float x, y;            /* PRESS, DRAG: the finger (any fixed frame, the screen's); JUMP: the position */
    int64_t t_us;          /* PRESS, DRAG, RELEASE, CANCEL: when PHP saw the touch (bm_hal_now_us()) */
    uint32_t seq;          /* JUMP, CONFIG: published back when applied (slate_scroll_pub_seq) */
    slate_scroll_pub_t *pub;   /* CONFIG: where to publish (a reference is taken at push); NULL: keep the one there */
} slate_scroll_cmd_t;

/* The transition of a SHOW (compose.h, Transitions). */
typedef struct {
    uint8_t kind;          /* slate_trans_kind_t; SLATE_TRANS_NONE: no transition (the rest ignored) */
    uint8_t dir;           /* slate_dir_t: the direction of the movement (FADE: ignored, any value) */
    uint8_t ease;          /* slate_ease_t */
    int32_t ms;            /* >= 1 */
} slate_trans_t;

typedef struct {
    slate_cmd_kind_t kind;
    uint32_t id;
    uint32_t parent;       /* ADD */
    slate_el_kind_t el;    /* NEW */
    slate_prop_t prop;     /* SET */
    slate_value_t v;       /* SET, SHOW */
    union {
        slate_rect_t r;    /* PIXELS */
        slate_trans_t tr;  /* SHOW */
    };
    const void *data;      /* SHOW: NUL-terminated name; PIXELS: the pixels; TWEEN, EASE: the spec; SCROLL: a
                              slate_scroll_cmd_t */
    bool keep;             /* SHOW: the scene it leaves may keep its caches (Scene::$keepCache) */
} slate_cmd_t;

#define SLATE_CMD_PIXELS_MAX (2u * 1024 * 1024)   /* bytes of one PIXELS command (a 1024 x 1024 canvas) */

typedef enum {
    SLATE_CMD_OK = 0,
    SLATE_CMD_NO_MEM,       /* out of memory */
    SLATE_CMD_BAD_PIXELS,   /* PIXELS without data, with an empty or out-of-range rectangle, or too big */
    SLATE_CMD_BAD_ARG,      /* TWEEN, EASE or SCROLL without data, a TWEEN with n out of 1..SLATE_TWEEN_PROPS_MAX, a
                               SCROLL with an op or an axis out of range; a SHOW with a transition out of range (kind,
                               dir, ease; ms < 1) */
} slate_cmd_result_t;

/* A SHOW's transition is acceptable: kind NONE, or a kind, a direction (any for FADE), an ease in range and ms >= 1. */
bool slate_trans_ok(const slate_trans_t *t);

typedef struct slate_cmdbuf slate_cmdbuf_t;

/* A command buffer; NULL when out of memory. */
slate_cmdbuf_t *slate_cmdbuf_new(void);
/* Frees it with every batch it still holds (nobody may take from it any more). NULL allowed. */
void slate_cmdbuf_free(slate_cmdbuf_t *b);

/* Appends a copy of *c (and of what it points to) to the pending batch. SLATE_CMD_OK, or the reason nothing was
   appended. */
slate_cmd_result_t slate_cmd_push(slate_cmdbuf_t *b, const slate_cmd_t *c);
/* Hands the pending batch over to the compositor. false when it was empty (nothing delivered). */
bool slate_cmd_deliver(slate_cmdbuf_t *b);
/* Commands in the pending batch (pushed, not yet delivered). */
size_t slate_cmd_pending(const slate_cmdbuf_t *b);

/* The oldest delivered batch: returns its number of commands and points *out at them (pointers inside resolved
   to the batch's own copies), or 0 with *out NULL when nothing is delivered. The batch stays valid until
   slate_cmd_release; take one batch at a time. */
size_t slate_cmd_take(slate_cmdbuf_t *b, const slate_cmd_t **out);
/* Gives back the batch taken last (no-op without one). */
void slate_cmd_release(slate_cmdbuf_t *b);

#endif
