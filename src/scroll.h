/* scroll.h - Slate's scroll physics. Pure C, no ESP-IDF, no PHP, no engine. Owned by the engine task: nothing here
   is thread-safe.

   One slate_scroll_t is the state of one scroll container (two axes, x and y). Times are int64 microseconds on
   any clock the caller chooses (the engine uses the vsync time of the frame); the physics never reads a clock.
   The struct is plain (280 bytes on a 64-bit host, no allocation): the owner embeds it.

   Position. The drawn position (slate_scroll_pos) is in 0..max per axis, plus the overscroll (rubber band) when
   `bounce` is on: negative before 0, above max after max. The overscroll never exceeds half the view.

   Drag. The content follows the finger exactly: pos = pos at press - (finger - finger at press), on the allowed
   axes only (BOTH, V: y, H: x). Past an edge with bounce the visible overscroll is d * (1 - 1 / (over * 0.55 / d + 1))
   (d = the view on the axis, over = how far the finger went past), at most d / 2; without bounce the content stops
   at the edge. The 8 px threshold before a drag is the caller's: slate_scroll_drag gets only the points after it.
   The FIRST drag point after a press only anchors the drag (the content does not move on that call); the later
   points move it relative to that anchor. Non-finite coordinates are ignored.

   Release. The velocity is the weighted (weights growing to the latest) least-squares slope of the drag points of
   the last 80 ms before the release, at most 4000 px/s per axis, 0 when no two points are left in that window (a pause before the lift: no fling). Then, per axis:
   - past an edge: critical spring (v0 = the speed of the stretched content: the finger velocity outward times the slope of the rubber band, 0.55 / (1 + 0.55 over / d)^2) x(t) = (x0 + (v0 + w x0) t) e^(-w t), w = 14/s, back to the edge, done when
     |x| < 0.5 px and |v| < 20 px/s;
   - snap > 0: target = pos + v * tau (tau = 325 ms) rounded to the nearest multiple of the snap step (the max edge
     is a snap point too), within 0..max; an OutCubic curve of 3 * |distance| / |v| (its initial speed is v), held
     to 150..600 ms; with |v| < 20 px/s: 200 ms to the nearest snap point; with snap_max n > 0 and |v| >= 20 px/s
     the target is first held to at most n snap points in the direction of the fling, counted from the snap point
     at or behind the release position (forward: the one at or below it; backward: at or above it); the snap points
     are the multiples of the step in 0..max and max (one step back from max is the multiple below it). The limit
     is one-sided: it never moves the target against the fling. Then within 0..max. With n = 1 a carousel moves one
     card per release whatever the speed, and a long drag never goes back to where the touch began;
   - otherwise exponential inertia, v <- v e^(-dt/tau), integrated exactly per step (the stop point does not depend
     on the step size), done under 20 px/s; past an edge it becomes the spring (bounce) or stops (no bounce).
   A release without drag does nothing, except when the content is past an edge or was stopped by the press while
   it was snapping: then it springs back / snaps (v = 0): a tap that stopped a snap resumes it (accepted).
   If the limits change under a motion it is retargeted: a snap target is clamped to the new max, a spring goes to
   the new edge, a fling continues and springs from the real overscroll (capped at half the view); the content
   never rests past an edge. A fling crossing an edge is split at the exact crossing time, so the overscroll does
   not depend on the frame phase.

   End. slate_scroll_take_end is true once when the motion that began at a release ended by itself, and once for a
   release after a drag that leaves no motion (the final position is the drag's). Not after a press that stopped
   the motion, not after a jump, not after a release without a drag at rest. */
#ifndef SLATE_SCROLL_PHYSICS_H
#define SLATE_SCROLL_PHYSICS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { SLATE_SCROLL_BOTH, SLATE_SCROLL_V, SLATE_SCROLL_H } slate_scroll_axis_t;   /* PHP ScrollAxis order */
typedef enum { SLATE_SC_IDLE, SLATE_SC_DRAG, SLATE_SC_FLING, SLATE_SC_SNAP, SLATE_SC_SPRING } slate_sc_state_t;

#define SLATE_SCROLL_POINTS 8

typedef struct {
    int64_t t_us;
    float x, y;                 /* the unconstrained content position at the point */
} slate_sc_point_t;

typedef struct {
    uint8_t mode;               /* 0 none, 1 fling, 2 snap, 3 spring (internal) */
    int snap;                   /* step in px, 0 = none */
    float view, max;
    float pos;                  /* drawn position, overscroll included */
    float v;                    /* fling velocity, px/s */
    float raw0, f0;             /* content position and finger at press, unconstrained */
    float a, b, c, d;           /* mode data: fling -, snap p0 / target / duration s, spring edge / sign / x0 / v0 */
    int64_t t0;                 /* start of the mode (fling: last step) */
} slate_sc_axis_t;

typedef struct slate_scroll {
    slate_scroll_axis_t axis;
    bool bounce;
    bool armed;                 /* pressed, no release yet */
    bool dragging;
    bool anchored;              /* the first drag point after the press has been taken */
    bool stopped;               /* the press stopped a motion */
    bool ended;                 /* the one-shot of slate_scroll_take_end */
    slate_sc_state_t state;
    slate_sc_axis_t ax[2];      /* 0 = x, 1 = y */
    int64_t last_now;           /* the latest time of a release or step: time never goes back */
    int npoints;                /* in pts, 0..SLATE_SCROLL_POINTS (a ring: the oldest is overwritten) */
    int head;                   /* next slot */
    int snap_max;               /* snap steps a release may travel (slate_scroll_snap_max), 0 = no limit */
    slate_sc_point_t pts[SLATE_SCROLL_POINTS];
} slate_scroll_t;

void slate_scroll_init(slate_scroll_t *s, slate_scroll_axis_t axis, bool bounce, int snap_x, int snap_y);
/* At most n (0..32767, 0 = no limit) snap steps per release on each axis with a snap step (see Release). */
void slate_scroll_snap_max(slate_scroll_t *s, int n);
/* The view and content sizes (px); max = max(0, content - view) on the axes `axis` allows, 0 on a locked axis (so
   jumps and the engine's scrollTo never move it either). At rest the position is brought into 0..max. */
void slate_scroll_limits(slate_scroll_t *s, float view_w, float view_h, float content_w, float content_h);
/* A finger down at (x, y): stops any motion (no end); true when there was motion. */
bool slate_scroll_press(slate_scroll_t *s, float x, float y, int64_t t_us);
/* A point of the drag (after the threshold, decided by the caller); ignored without a press. */
void slate_scroll_drag(slate_scroll_t *s, float x, float y, int64_t t_us);
/* The finger up: fling, snap or spring, or IDLE. */
void slate_scroll_release(slate_scroll_t *s, int64_t t_us);
/* The touch is lost (the finger's Released will not come): a release with velocity 0 (a spring back from past an
   edge, a snap, or rest; a drag still ends, slate_scroll_take_end). Nothing without a press. */
void slate_scroll_cancel(slate_scroll_t *s, int64_t t_us);
/* Sets the position (clamped to 0..max); stops all motion; no end. */
void slate_scroll_jump(slate_scroll_t *s, float x, float y);
/* Advances the motion to now_us; true while an inertia, snap or spring is running. */
bool slate_scroll_step(slate_scroll_t *s, int64_t now_us);
/* One-shot: the motion that began at a release ended. */
bool slate_scroll_take_end(slate_scroll_t *s);
void slate_scroll_pos(const slate_scroll_t *s, float *x, float *y);
void slate_scroll_max(const slate_scroll_t *s, float *mx, float *my);
slate_sc_state_t slate_scroll_state(const slate_scroll_t *s);

#endif
