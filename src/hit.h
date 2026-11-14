/* hit.h - Slate's hit table: where the touchable elements of the frame shown are, for the PHP task to route a touch
   without a lock. Pure C, no ESP-IDF and no PHP (memory through the scene allocator and the events HAL of ext-events).

   Touchable. An element is touchable when its TOUCHABLE property is set (scene.h; the PHP layer sets it for
   $touchable or for an element with a callback). The scene counts the touchables of every subtree (scene.h,
   slate_el_touchables): a root with none costs the engine nothing per frame.

   The table (slate_hit_build). The engine walks the root shown in draw order (bottom to top, a parent before its
   children), skipping hidden elements (visible false or opacity 0, themselves or an ancestor), branches whose
   clip (a group that clips, a scroll's view, the screen) leaves nothing and subtrees without touchables, and writes
   one entry per touchable element whose box is not empty:
     - id: the element's id (the ids of the commands, cmd.h);
     - box (x0, y0, x1, y1), half open, screen pixels: the element's extent on screen (its geometry rounded outward,
       without shadow, blur or anti-aliasing fringe; a GROUP: the union of its visible descendants' extents; a GROUP
       that clips: its clip box, children or not (a touch zone; a scroll: its view)), grown by its HIT_PAD on every
       side, then cut by the clip of every ancestor (groups that clip, scroll views) and by the screen;
     - origin (ox, oy): the screen position of the element's own (x, y) (rounded to whole pixels), the drawn
       positions of the scrolls among its ancestors included (a CIRCLE's or ARC's centre, a LINE's first end);
     - scroll: the index of the entry of the nearest touchable scroll among its ancestors, -1 for none (that entry
       always comes before it);
     - flags: SLATE_HIT_SCROLL for a GROUP that is a scroll, with SLATE_HIT_AXIS_X / SLATE_HIT_AXIS_Y for each axis
       it can move now (its axis allows it and its limit there is above 0, scroll.h).
   So the touchable on top is the entry with the highest index whose box contains the point: a touchable child of a
   touchable group comes after the group and wins inside its own box; children that are not touchable are part of
   their touchable ancestor's box. At most SLATE_HIT_MAX entries (more are left out).

   Publication (slate_hit_pub_t). One block, counted references (the PHP layer holds one, the engine one: see
   slate_engine_set_hit_pub, engine.h), freed by the last slate_hit_pub_put. It holds two tables and a sequence
   number: the table seq & 1 is the published one, the other is the engine's to write. The engine (the only writer)
   fills the free table (slate_hit_pub_table, slate_hit_build) and publishes it (slate_hit_pub_publish: its entry
   count, then seq + 1 with release order); the table published before becomes the free one, written again only
   after that. A reader (any task, no lock) reads seq (acquire), reads the table seq & 1, then reads seq again: the
   copy is good when seq did not change, else it starts over. The entries themselves are plain loads and stores
   raced against the writer (formally a data race in C11, as in every seqlock and in slate_scroll_pub_t): the fences
   order them, and a copy taken while the writer wrote is never handed out (the engine published meanwhile, and may
   be writing the table it was reading). A reader never waits for the writer (the engine may be preempted by the reader's task on
   the same core): the table it reads is never written while seq stays the same. Several entries of one snapshot:
   slate_hit_find and slate_hit_lookup give the seq of their snapshot, slate_hit_at reads another entry of it and
   fails when seq moved on (start over then). The block (atomics) is in internal RAM; the tables come from the scene
   allocator (scene.h; PSRAM on the board), a capacity of SLATE_HIT_CAP_MIN entries doubled as needed; a table
   outgrown is kept until the block is freed (a reader may still be reading it), so a block holds at most about
   twice the largest capacity per table. Out of memory: the build fails and the table published stays. */
#ifndef SLATE_HIT_H
#define SLATE_HIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scene.h"

#define SLATE_HIT_SCROLL 0x01u   /* the entry is a scroll (a GROUP with scroll state) */
#define SLATE_HIT_AXIS_X 0x02u   /* the scroll can move horizontally now */
#define SLATE_HIT_AXIS_Y 0x04u   /* the scroll can move vertically now */

#define SLATE_HIT_CAP_MIN 32     /* entries of a table's first capacity */
#define SLATE_HIT_MAX 32767      /* entries of one table at most */

/* One entry: 20 bytes. */
typedef struct {
    uint32_t id;              /* element id (as in cmd.h) */
    int16_t x0, y0, x1, y1;   /* screen box, half open: extent (+children), + hit_pad, cut by ancestors' clips */
    int16_t ox, oy;           /* screen origin, ancestor scroll positions included */
    int16_t scroll;           /* index of the nearest touchable Scroll ancestor's entry, -1: none */
    uint8_t flags;            /* SLATE_HIT_SCROLL, SLATE_HIT_AXIS_X, SLATE_HIT_AXIS_Y */
} slate_hit_t;

typedef struct slate_hit_pub slate_hit_pub_t;   /* refcounted, double-buffered, seq */

/* A new block (one reference, both tables empty, seq 0); NULL when out of memory. Any task. */
slate_hit_pub_t *slate_hit_pub_new(void);
/* Takes / drops a reference (NULL allowed); the last drop frees the block and its tables. Any task. */
void slate_hit_pub_ref(slate_hit_pub_t *p);
void slate_hit_pub_put(slate_hit_pub_t *p);
/* The references held now (for tests). */
int slate_hit_pub_refs(const slate_hit_pub_t *p);

/* ---- readers: any task, no lock ---- */

/* Search from the top (the highest index first) for the first entry whose box contains (x, y); returns its index
   with *out a copy of it, or -1 (*out untouched) when none does. *seq (NULL allowed): the seq of the snapshot
   searched, for slate_hit_at. */
int slate_hit_find(slate_hit_pub_t *p, int x, int y, slate_hit_t *out, uint32_t *seq);
/* The entry of element id in the table published (an id has one entry at most): its index with *out a copy, or -1
   when it has none (not touchable, hidden, off the screen or its box empty). *seq as for slate_hit_find. */
int slate_hit_lookup(slate_hit_pub_t *p, uint32_t id, slate_hit_t *out, uint32_t *seq);
/* A copy of entry i of the snapshot seq (from slate_hit_find or slate_hit_lookup), e.g. to follow the scroll
   chain; false when the table published is no longer that snapshot (start over) or i is out of range. */
bool slate_hit_at(slate_hit_pub_t *p, uint32_t seq, int i, slate_hit_t *out);
/* A copy of the whole table published into out (at most max entries); returns its entry count (possibly more than
   max), *seq as above. */
int slate_hit_copy(slate_hit_pub_t *p, slate_hit_t *out, int max, uint32_t *seq);

/* ---- the writer: one task (the engine's) ---- */

/* The free table with room for at least need entries (grown to the next power-of-two multiple of
   SLATE_HIT_CAP_MIN, the old one kept until the block goes); NULL when the scene allocator refuses (nothing changed).
   Its content is the writer's: fill it, then slate_hit_pub_publish. */
slate_hit_t *slate_hit_pub_table(slate_hit_pub_t *p, int need);
/* Publishes the free table with its first n entries (0: an empty table, no slate_hit_pub_table needed). */
void slate_hit_pub_publish(slate_hit_pub_t *p, int n);
/* The bytes the scene allocator is asked for a table of cap entries (for tests). */
size_t slate_hit_table_bytes(int cap);

/* The ids of the touchable elements, by element (the engine's: the scene does not know ids). An open-addressing
   table from the scene allocator, grown as needed. */
typedef struct {
    const slate_el_t **el;    /* NULL: free slot */
    uint32_t *id;
    uint32_t cap, n;
} slate_hit_ids_t;
/* Notes (or changes) el's id; false when out of memory (nothing noted). */
bool slate_hit_ids_put(slate_hit_ids_t *m, const slate_el_t *el, uint32_t id);
/* Forgets el (no-op when it is not there). */
void slate_hit_ids_del(slate_hit_ids_t *m, const slate_el_t *el);
/* el's id, 0 when it is not there. */
uint32_t slate_hit_ids_get(const slate_hit_ids_t *m, const slate_el_t *el);
/* Frees its memory (it is empty after). */
void slate_hit_ids_free(slate_hit_ids_t *m);

/* Builds the table of root (the root shown, see "The table" above) on a screen of sw x sh pixels into p's free
   table and publishes it. ids gives the elements' ids (an element without one is left out). Returns the entries
   published, or -1 when the scene allocator refused a larger table (the table published stays). Engine task. */
int slate_hit_build(slate_hit_pub_t *p, slate_el_t *root, int sw, int sh, const slate_hit_ids_t *ids);

#endif
