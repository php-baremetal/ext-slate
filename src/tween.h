/* tween.h - Slate's easing curves and tweens. Pure C, no ESP-IDF and no PHP. Owned by the compositor task:
   nothing here is thread-safe.

   A tween moves one or more properties of an element from a value to another over a time, with an easing curve,
   an optional delay, repetitions and yoyo. It knows nothing of elements: the engine samples it with the time of
   the frame and writes the values it gives into the element.

   Time. Times are int64 microseconds on any clock the caller chooses (the engine uses the vsync time of the
   frame). A sample is computed from the time elapsed since the start, never accumulated from frame to frame: a
   tween sampled every 16 ms or every 100 ms gives the same value at the same instant, and an endless tween stays
   exact after any time. The delay comes once, before the first run. Run k (0-based) covers
   [start + delay + k * ms, start + delay + (k + 1) * ms); with yoyo, the odd runs play the curve backwards
   (value at ease(1 - p)), so the run goes from `to` back to `from`. A tween with repeat r >= 0 is done at
   start + delay + (r + 1) * ms (paused time added), and then gives its final values exactly: `to`, or `from` when
   yoyo is on and r is odd. Repeat -1 never ends.

   Values. NUM: from + (to - from) * eased, exactly `from` at eased 0 and exactly `to` at eased 1 (eased may go
   past 0..1 with OutBack and OutElastic). COLOR (argb): per 8-bit channel (A, R, G, B), c = from + round(
   (to - from) * eased), the step rounded to the nearest integer with halves away from zero (so towards the
   target: 0x00 -> 0xFF at 1/2 is 0x80, 0xFF -> 0x80 at 1/2 is 0xBF), then clamped to 0..255. Whole-pixel
   properties (blur) are the caller's to round.

   Pause. slate_tween_pause freezes the tween at the time given (samples give the values of that instant);
   slate_tween_resume moves the start by the time spent paused, so the pause does not count and the end moves by
   as much. Pausing a paused tween, resuming a running one, and pausing or resuming a done one do nothing.

   Chains. A tween created with slate_tween_new_chained is not started: its samples give nothing (n = 0, false)
   and it has no end. slate_tween_set_next(a, b) appends b at the end of a's chain (after the last tween reached
   from a by next); b starts when that tween is done, at its exact end time (not at the time of the sample that
   saw it done): the first sample of it at or after its end starts b. When that tween is already done (a sample
   has seen it done, so its end is past), b starts at the time of its own first sample. A chained tween paused
   before it starts stays paused from its start (the pause counts from the later of the two).

   Memory. A tween is one block from the scene allocator (slate_scene_mem): a 40-byte record (on the ESP32-S3 as on a
   64-bit host) and 16 bytes per property. slate_tween_free frees one tween, not its chain. */
#ifndef SLATE_TWEEN_H
#define SLATE_TWEEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scene.h"

/* Easing curves, in the order of the PHP enum Ease. */
typedef enum {
    SLATE_EASE_LINEAR,
    SLATE_EASE_IN_QUAD,
    SLATE_EASE_OUT_QUAD,
    SLATE_EASE_IN_OUT_QUAD,
    SLATE_EASE_IN_CUBIC,
    SLATE_EASE_OUT_CUBIC,
    SLATE_EASE_IN_OUT_CUBIC,
    SLATE_EASE_OUT_BACK,      /* c1 = 1.70158: goes past 1 (about 1.1) before settling */
    SLATE_EASE_OUT_BOUNCE,    /* n1 = 7.5625, d1 = 2.75: touches 1 three times before the end */
    SLATE_EASE_OUT_ELASTIC,   /* c4 = 2 pi / 3: oscillates around 1 */
} slate_ease_t;
#define SLATE_EASE_COUNT 10

/* The curve at t (the standard Penner / easings.net formulas): t <= 0 gives exactly 0, t >= 1 exactly 1. */
float slate_ease(slate_ease_t e, float t);

/* One animated property: from -> to (NUM: num; COLOR: argb, per channel). In a sample, `to` holds the current
   value and `from` is left as it was. */
typedef struct {
    slate_prop_t prop;
    slate_val_type_t type;    /* SLATE_VAL_NUM or SLATE_VAL_COLOR */
    union { float num; uint32_t argb; } from, to;
} slate_tween_prop_t;

typedef struct slate_tween slate_tween_t;

/* The bytes of a tween with n properties. */
size_t slate_tween_size(int n);

/* A tween of n properties (copied, 0..255; their types NUM or COLOR) started at start_us, ms (>= 1) long per run,
   after delay_ms (>= 0), with repeat (-1 = always, else the number of runs after the first) and yoyo. NULL on a
   bad argument or out of memory. */
slate_tween_t *slate_tween_new(const slate_tween_prop_t *props, int n, int64_t start_us, int ms, slate_ease_t ease,
                               int delay_ms, int repeat, bool yoyo);
/* The same, not started: it starts when the tween it is chained after is done (slate_tween_set_next). */
slate_tween_t *slate_tween_new_chained(const slate_tween_prop_t *props, int n, int ms, slate_ease_t ease,
                                       int delay_ms, int repeat, bool yoyo);
/* Frees t (NULL allowed); its next tween is left alone. */
void slate_tween_free(slate_tween_t *t);

/* Samples t at now_us: writes the current value of each property into out (room for slate_tween_count(t)), their
   count into *n, and returns true when t is done (it then starts its next tween, at its end time). A tween not
   started writes nothing (*n = 0) and returns false. */
bool slate_tween_sample(slate_tween_t *t, int64_t now_us, slate_tween_prop_t *out, int *n);

/* Freezes t at now_us / starts it again from where it was, moving its end by the time spent paused. */
void slate_tween_pause(slate_tween_t *t, int64_t now_us);
void slate_tween_resume(slate_tween_t *t, int64_t now_us);
bool slate_tween_paused(const slate_tween_t *t);

/* Removes the property prop from t (the order of the others kept); false when t does not animate it. A tween
   without properties still runs (and ends, and starts its next). */
bool slate_tween_drop_prop(slate_tween_t *t, slate_prop_t prop);
bool slate_tween_has_prop(const slate_tween_t *t, slate_prop_t prop);
int slate_tween_count(const slate_tween_t *t);
/* t's properties (slate_tween_count of them), in place: the engine sets the from values of a chained tween when it
   starts (from the values its predecessor left). */
slate_tween_prop_t *slate_tween_props(slate_tween_t *t);
/* The values t ends with (`to`, or `from` with yoyo and an odd repeat; repeat -1: `to`) into out, their count into
   *n: what stop(to_end) writes. */
void slate_tween_final(const slate_tween_t *t, slate_tween_prop_t *out, int *n);

/* Appends next (made with slate_tween_new_chained, not started) at the end of t's chain; see Chains above.
   false (nothing done) when next has started (or is about to), is already in t's chain, or t is in next's chain.
   next NULL: t's own next is unlinked (it, and what follows it, are left alone and never started by t). */
bool slate_tween_set_next(slate_tween_t *t, slate_tween_t *next);
slate_tween_t *slate_tween_next(const slate_tween_t *t);

bool slate_tween_started(const slate_tween_t *t);
/* true once a sample has seen t done. */
bool slate_tween_done(const slate_tween_t *t);
/* The time t is done (paused time included so far); INT64_MAX when that time is not known: repeat -1, a tween not
   started, or while paused (slate_tween_active tells them apart). */
int64_t slate_tween_end_us(const slate_tween_t *t);
/* true while t needs frames: started (or chained after a tween already done: it starts at its next sample), not
   done, not paused (repeat -1 included). */
bool slate_tween_active(const slate_tween_t *t);
/* true for repeat -1: t never ends. */
bool slate_tween_endless(const slate_tween_t *t);

/* Writes n values (from slate_tween_sample or slate_tween_final: each property's `to`) into e with slate_el_set, as
   a change from PHP would (d, sw, sh as there: dirty areas, caches): X and Y together as one move (slate_el_move),
   FILL and COLOR as a solid paint, BORDER_COLOR as a colour, BLUR rounded to whole pixels (by slate_el_set). A value
   e does not take is skipped. */
void slate_tween_write(slate_el_t *e, const slate_tween_prop_t *vals, int n, slate_dirty_t *d, int sw, int sh);

#endif
