/* cmd.h - Slate's command buffer: how the PHP task changes the scene the compositor task owns.
   Pure C, no ESP-IDF and no PHP (locks and memory through the events HAL of ext-events).

   The PHP task never touches the scene: it appends commands (slate_cmd_push) to a pending batch and, when it
   returns to the event loop (or at show()), hands the whole batch over (slate_cmd_deliver). The compositor task
   takes the delivered batches one at a time, oldest first (slate_cmd_take), applies every command of a batch
   before it composes a frame, and gives the batch back (slate_cmd_release). A command pushed while the engine
   is taking a batch goes into the pending batch, delivered later: a batch is never seen half written.

   Elements are named by ids the PHP task assigns (uint32, 0 = none); the engine maps them to its elements.
   Everything a command points to is copied into the batch at push (texts, the gradient and its stops, the scene
   name, canvas pixels): the caller's memory may go as soon as push returns. Fonts are kept by pointer (entries
   of display_fonts[], which live as long as the firmware); an image (SET of SLATE_VAL_IMAGE) by a reference
   taken at push and given back when the batch is released or freed (images.h).

   Threading: slate_cmdbuf_new, slate_cmdbuf_free, slate_cmd_push, slate_cmd_deliver and slate_cmd_pending belong
   to one task (PHP's); slate_cmd_take and slate_cmd_release to another (the compositor's). The two sides only
   meet in deliver / take / release, under a short lock. Memory: external RAM (PSRAM on the board). */
#ifndef SLATE_CMD_H
#define SLATE_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scene.h"

typedef enum {
    SLATE_CMD_NEW = 1,   /* id: a new element of kind el (the defaults of slate_el_new) */
    SLATE_CMD_SET,       /* id: property prop = v (slate_el_set) */
    SLATE_CMD_ADD,       /* id (child) on top of the children of parent (a group) */
    SLATE_CMD_REMOVE,    /* id: detached from its parent; it stays alive */
    SLATE_CMD_FREE,      /* id: destroyed (a group's children are detached first and stay alive); the id is free */
    SLATE_CMD_SHOW,      /* id: the root group of the scene to show (0: none); data: its name (for messages, may be
                            NULL); v: the background, a COLOR (another type: black) */
    SLATE_CMD_PIXELS,    /* id: a canvas; r: a rectangle in its own pixels (cut to the canvas when applied);
                            data: the pixels of r, RGB565 native, row after row (stride r.x1 - r.x0), copied into
                            the canvas and marked dirty. data is required, 0 <= x0 < x1 <= 32767 (likewise y) and
                            the pixels at most SLATE_CMD_PIXELS_MAX bytes: the PHP layer sends a canvas's own
                            rectangle (or the changed part of it) */
} slate_cmd_kind_t;

typedef struct {
    slate_cmd_kind_t kind;
    uint32_t id;
    uint32_t parent;       /* ADD */
    slate_el_kind_t el;    /* NEW */
    slate_prop_t prop;     /* SET */
    slate_value_t v;       /* SET, SHOW */
    slate_rect_t r;        /* PIXELS */
    const void *data;      /* SHOW: NUL-terminated name; PIXELS: the pixels */
} slate_cmd_t;

#define SLATE_CMD_PIXELS_MAX (2u * 1024 * 1024)   /* bytes of one PIXELS command (a 1024 x 1024 canvas) */

typedef enum {
    SLATE_CMD_OK = 0,
    SLATE_CMD_NO_MEM,       /* out of memory */
    SLATE_CMD_BAD_PIXELS,   /* PIXELS without data, with an empty or out-of-range rectangle, or too big */
} slate_cmd_result_t;

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
