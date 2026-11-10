/* Dirty regions of Slate: see dirty.h. */
#include "slate_speed.h"   /* first: run for every change of every frame */
#include <stdbool.h>

#include "dirty.h"

static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }

slate_rect_t slate_rect_and(slate_rect_t a, slate_rect_t b)
{
    slate_rect_t r = { imax(a.x0, b.x0), imax(a.y0, b.y0), imin(a.x1, b.x1), imin(a.y1, b.y1) };
    return r;
}

slate_rect_t slate_rect_or(slate_rect_t a, slate_rect_t b)
{
    if (slate_rect_empty(a)) {
        return b;
    }
    if (slate_rect_empty(b)) {
        return a;
    }
    slate_rect_t r = { imin(a.x0, b.x0), imin(a.y0, b.y0), imax(a.x1, b.x1), imax(a.y1, b.y1) };
    return r;
}

int slate_rect_contains(slate_rect_t a, slate_rect_t b)
{
    return slate_rect_empty(b) || (b.x0 >= a.x0 && b.y0 >= a.y0 && b.x1 <= a.x1 && b.y1 <= a.y1);
}

static long ar(slate_rect_t r) { return slate_rect_empty(r) ? 0 : (long)(r.x1 - r.x0) * (r.y1 - r.y0); }
static bool meet(slate_rect_t a, slate_rect_t b) { return !slate_rect_empty(slate_rect_and(a, b)); }

/* Pixels composed for nothing when a and b become their bounding box. */
static long waste(slate_rect_t a, slate_rect_t b)
{
    return ar(slate_rect_or(a, b)) - ar(a) - ar(b) + ar(slate_rect_and(a, b));
}

/* Removes rect i. */
static void drop(slate_dirty_t *d, int i) { d->r[i] = d->r[--d->n]; }

/* u grown until it meets no rect of d (the ones it meets are taken in and removed). */
static slate_rect_t absorb(slate_dirty_t *d, slate_rect_t u)
{
    for (int i = 0; i < d->n;) {
        if (meet(d->r[i], u)) {
            u = slate_rect_or(u, d->r[i]);
            drop(d, i);
            i = 0;
        } else {
            i++;
        }
    }
    return u;
}

/* The union of r and rect k meets no other rect of d. */
static bool alone(const slate_dirty_t *d, slate_rect_t u, int k)
{
    for (int i = 0; i < d->n; i++) {
        if (i != k && meet(d->r[i], u)) {
            return false;
        }
    }
    return true;
}

#define PEND_MAX 32   /* pieces waiting to be added; past it a piece takes in what it meets instead of splitting */

void slate_dirty_add(slate_dirty_t *d, slate_rect_t r, int screen_w, int screen_h)
{
    if (!d) {
        return;
    }
    r = slate_rect_and(r, (slate_rect_t){ 0, 0, screen_w, screen_h });
    if (slate_rect_empty(r)) {
        return;
    }
    r.x0 &= ~1;
    r.x1 = imin((r.x1 + 1) & ~1, screen_w);
    slate_rect_t pend[PEND_MAX];
    int np = 0;
    pend[np++] = r;
    while (np > 0) {
        r = pend[--np];
        bool done = false;   /* r is in the set, or its pieces are waiting */
        for (int i = 0; i < d->n && !done;) {
            slate_rect_t a = d->r[i];
            if (slate_rect_contains(a, r)) {
                done = true;
                break;
            }
            bool over = meet(a, r), cheap = waste(a, r) <= SLATE_DIRTY_COST;
            slate_rect_t u = slate_rect_or(a, r);
            if (cheap && alone(d, u, i)) {
                r = u;   /* cheaper as one: merged, and looked at again against the others */
                drop(d, i);
                i = 0;
                continue;
            }
            if (!over) {
                i++;
                continue;
            }
            if (np + 4 > PEND_MAX) {
                r = absorb(d, u);
                break;
            }
            /* r minus a: up to four pieces, each added on its own */
            slate_rect_t c = slate_rect_and(a, r);
            slate_rect_t piece[4] = {
                { r.x0, r.y0, r.x1, c.y0 }, { r.x0, c.y1, r.x1, r.y1 },   /* above, below */
                { r.x0, c.y0, c.x0, c.y1 }, { c.x1, c.y0, r.x1, c.y1 },   /* left, right */
            };
            for (int k = 0; k < 4; k++) {
                if (!slate_rect_empty(piece[k])) {
                    pend[np++] = piece[k];
                }
            }
            done = true;
        }
        if (done) {
            continue;
        }
        while (d->n == SLATE_DIRTY_MAX) {
            /* full: the cheapest union of two (r included) */
            int bi = 0, bj = -1;
            long best = waste(d->r[0], r);
            for (int i = 0; i < d->n; i++) {
                long w = waste(d->r[i], r);
                if (w < best) {
                    best = w;
                    bi = i;
                    bj = -1;
                }
                for (int j = i + 1; j < d->n; j++) {
                    w = waste(d->r[i], d->r[j]);
                    if (w < best) {
                        best = w;
                        bi = i;
                        bj = j;
                    }
                }
            }
            if (bj < 0) {
                slate_rect_t u = slate_rect_or(r, d->r[bi]);
                drop(d, bi);
                r = absorb(d, u);
            } else {
                slate_rect_t u = slate_rect_or(d->r[bi], d->r[bj]);
                drop(d, bj);
                drop(d, bi);
                u = absorb(d, u);
                if (meet(u, r)) {
                    r = absorb(d, slate_rect_or(u, r));
                } else {
                    d->r[d->n++] = u;
                }
            }
        }
        d->r[d->n++] = r;
    }
}
