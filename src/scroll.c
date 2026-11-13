/* scroll.c - Slate's scroll physics (see scroll.h). Pure C, no ESP-IDF, no PHP. */
#include "scroll.h"

#include <math.h>
#include <string.h>

#include "tween.h"

#define TAU_S 0.325f
#define OMEGA 14.0f
#define STOP_V 20.0f
#define MAX_V 4000.0f
#define WINDOW_US 80000
#define SNAP_MIN_US 150000
#define SNAP_MAX_US 600000
#define SNAP_SLOW_US 200000

enum { M_NONE, M_FLING, M_SNAP, M_SPRING };

static bool axis_ok(const slate_scroll_t *s, int i)
{
    return s->axis == SLATE_SCROLL_BOTH || (s->axis == SLATE_SCROLL_V ? i == 1 : i == 0);
}

static float rubber(float over, float d)
{
    if (d <= 0.0f || over <= 0.0f) return 0.0f;
    float r = d * (1.0f - 1.0f / (over * 0.55f / d + 1.0f));
    return r > d * 0.5f ? d * 0.5f : r;
}

/* The finger overshoot that shows r px of overscroll (the inverse of rubber, r < d / 2). */
static float unrubber(float r, float d)
{
    if (d <= 0.0f || r <= 0.0f) return 0.0f;
    if (r >= d * 0.5f) r = d * 0.5f;
    return (d / 0.55f) * (1.0f / (1.0f - r / d) - 1.0f);
}

/* d(visible)/d(finger) of the rubber band at the stretch `shown`: the speed of the stretched content is the
   finger's times 0.55 / (1 + 0.55 over / d)^2 (the derivative of the mapping), over being the finger overshoot;
   0 at the cap. */
static float band_slope(float shown, float d)
{
    if (d <= 0.0f || shown >= d * 0.5f) return 0.0f;
    float k = 1.0f / (1.0f + 0.55f * unrubber(shown, d) / d);
    return 0.55f * k * k;
}

static void refresh(slate_scroll_t *s)
{
    if (s->dragging) { s->state = SLATE_SC_DRAG; return; }
    int best = M_NONE;
    for (int i = 0; i < 2; i++) {
        int m = s->ax[i].mode;
        if (m == M_SPRING) best = M_SPRING;
        else if (m == M_SNAP && best != M_SPRING) best = M_SNAP;
        else if (m == M_FLING && best == M_NONE) best = M_FLING;
    }
    s->state = best == M_SPRING ? SLATE_SC_SPRING : best == M_SNAP ? SLATE_SC_SNAP
             : best == M_FLING ? SLATE_SC_FLING : SLATE_SC_IDLE;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* The snap points of an axis: the multiples of the step in 0..max (index k: k * step), then max when it is not one
   (the last index). */
static int snap_last(const slate_sc_axis_t *a)
{
    int k = (int)floorf(a->max / (float)a->snap);
    return (float)k * (float)a->snap < a->max ? k + 1 : k;
}

static float snap_value(const slate_sc_axis_t *a, int i)
{
    float v = (float)i * (float)a->snap;
    return v > a->max ? a->max : v;
}

/* The index of the last snap point at or below p (dir > 0), or of the first at or above it (dir < 0), p held to
   0..max. */
static int snap_behind(const slate_sc_axis_t *a, float p, float dir)
{
    p = clampf(p, 0.0f, a->max);
    int last = snap_last(a);
    int i = dir > 0.0f ? (int)floorf(p / (float)a->snap) : (int)ceilf(p / (float)a->snap);
    if (i > last) i = last;
    if (i < 0) i = 0;
    return i;
}

void slate_scroll_init(slate_scroll_t *s, slate_scroll_axis_t axis, bool bounce, int snap_x, int snap_y)
{
    memset(s, 0, sizeof *s);
    s->axis = axis;
    s->bounce = bounce;
    s->ax[0].snap = snap_x > 0 ? snap_x : 0;
    s->ax[1].snap = snap_y > 0 ? snap_y : 0;
    s->state = SLATE_SC_IDLE;
}

void slate_scroll_snap_max(slate_scroll_t *s, int n)
{
    s->snap_max = n > 0 ? (n > 32767 ? 32767 : n) : 0;
}

void slate_scroll_limits(slate_scroll_t *s, float view_w, float view_h, float content_w, float content_h)
{
    float view[2] = { view_w, view_h }, content[2] = { content_w, content_h };
    for (int i = 0; i < 2; i++) {
        slate_sc_axis_t *a = &s->ax[i];
        a->view = view[i] > 0.0f ? view[i] : 0.0f;
        a->max = axis_ok(s, i) && content[i] - a->view > 0.0f ? content[i] - a->view : 0.0f;   /* locked axis: 0 */
        if (s->state == SLATE_SC_IDLE && !s->armed) a->pos = clampf(a->pos, 0.0f, a->max);
    }
}

static void stop_all(slate_scroll_t *s)
{
    for (int i = 0; i < 2; i++) { s->ax[i].mode = M_NONE; s->ax[i].v = 0.0f; }
    s->dragging = false;
    s->armed = false;
    s->npoints = 0;
    s->head = 0;
}

static void add_point(slate_scroll_t *s, int64_t t_us, float x, float y)
{
    s->pts[s->head].t_us = t_us;
    s->pts[s->head].x = x;
    s->pts[s->head].y = y;
    s->head = (s->head + 1) % SLATE_SCROLL_POINTS;
    if (s->npoints < SLATE_SCROLL_POINTS) s->npoints++;
}

bool slate_scroll_press(slate_scroll_t *s, float x, float y, int64_t t_us)
{
    (void)t_us;
    if (!isfinite(x) || !isfinite(y)) return false;
    bool moving = s->state == SLATE_SC_FLING || s->state == SLATE_SC_SNAP || s->state == SLATE_SC_SPRING;
    stop_all(s);
    float f[2] = { x, y };
    for (int i = 0; i < 2; i++) {
        slate_sc_axis_t *a = &s->ax[i];
        a->f0 = f[i];
        if (a->pos < 0.0f) a->raw0 = -unrubber(-a->pos, a->view);
        else if (a->pos > a->max) a->raw0 = a->max + unrubber(a->pos - a->max, a->view);
        else a->raw0 = a->pos;
    }
    s->armed = true;
    s->anchored = false;
    s->stopped = moving;
    s->ended = false;
    refresh(s);
    return moving;
}

void slate_scroll_drag(slate_scroll_t *s, float x, float y, int64_t t_us)
{
    if (!s->armed || !isfinite(x) || !isfinite(y)) return;
    float f[2] = { x, y }, raw[2];
    if (!s->anchored) {                 /* the first point only anchors the drag: the content does not move */
        for (int i = 0; i < 2; i++) { s->ax[i].f0 = f[i]; raw[i] = s->ax[i].raw0; }
        s->anchored = true;
        s->dragging = true;
        add_point(s, t_us, raw[0], raw[1]);
        refresh(s);
        return;
    }
    for (int i = 0; i < 2; i++) {
        slate_sc_axis_t *a = &s->ax[i];
        raw[i] = a->raw0;
        if (!axis_ok(s, i)) continue;
        float r = a->raw0 - (f[i] - a->f0);
        raw[i] = r;
        if (r < 0.0f) a->pos = s->bounce ? -rubber(-r, a->view) : 0.0f;
        else if (r > a->max) a->pos = s->bounce ? a->max + rubber(r - a->max, a->view) : a->max;
        else a->pos = r;
    }
    add_point(s, t_us, raw[0], raw[1]);
    s->dragging = true;
    refresh(s);
}

/* Weighted (weights 1, 2, 3 ... towards the latest) least-squares slope of the points of the last 80 ms. */
static void velocity(const slate_scroll_t *s, int64_t release_us, float out[2])
{
    out[0] = out[1] = 0.0f;
    if (s->npoints < 2) return;
    int last = (s->head + SLATE_SCROLL_POINTS - 1) % SLATE_SCROLL_POINTS;
    int64_t tl = s->pts[last].t_us;
    if (release_us - tl > WINDOW_US) return;     /* (the loop below also keeps only the last 80 ms before the release) */
    double sw = 0, st = 0, sx = 0, sy = 0, stt = 0, stx = 0, sty = 0;
    int n = 0;
    while (n < s->npoints && release_us - s->pts[(s->head + 2 * SLATE_SCROLL_POINTS - 1 - n) % SLATE_SCROLL_POINTS].t_us <= WINDOW_US) n++;
    for (int k = 0; k < n; k++) {      /* k = 0 is the oldest of the window */
        const slate_sc_point_t *p = &s->pts[(s->head + 2 * SLATE_SCROLL_POINTS - n + k) % SLATE_SCROLL_POINTS];
        double w = k + 1, t = (double)(p->t_us - tl) * 1e-6;
        sw += w; st += w * t; sx += w * p->x; sy += w * p->y;
        stt += w * t * t; stx += w * t * p->x; sty += w * t * p->y;
    }
    if (n < 2) return;
    double den = sw * stt - st * st;
    if (den <= 1e-12) return;
    out[0] = (float)((sw * stx - st * sx) / den);
    out[1] = (float)((sw * sty - st * sy) / den);
}

static void start_spring(slate_sc_axis_t *a, float edge, float sign, float x0, float v0, int64_t t)
{
    float cap = a->view * 0.5f;
    if (x0 > cap) x0 = cap;
    if (v0 < -OMEGA * x0) v0 = -OMEGA * x0;     /* never cross the edge back */
    a->mode = M_SPRING;
    a->a = edge; a->b = sign; a->c = x0; a->d = v0;
    a->t0 = t;
    a->v = 0.0f;
}

static bool start_snap(slate_sc_axis_t *a, float v, int64_t t, int snap_max)
{
    float target, dur_us;
    float snap = (float)a->snap;
    if (fabsf(v) < STOP_V) {
        float lo = floorf(a->pos / snap) * snap, hi = lo + snap;
        target = (a->pos - lo <= hi - a->pos) ? lo : hi;
        dur_us = SNAP_SLOW_US;
    } else {
        target = roundf((a->pos + v * TAU_S) / snap) * snap;
        dur_us = 0.0f;
    }
    float nat = a->pos + v * TAU_S;
    if (fabsf(v) >= STOP_V && fabsf(a->max - nat) < fabsf(target - nat)) target = a->max;
    else if (fabsf(v) < STOP_V && fabsf(a->max - a->pos) < fabsf(target - a->pos)) target = a->max;
    if (snap_max > 0 && fabsf(v) >= STOP_V) {
        /* at most snap_max snap points in the direction of the fling, from the snap point at or behind the release
           position; one-sided: the limit never moves the target against the fling */
        int last = snap_last(a), i = snap_behind(a, a->pos, v);
        if (v > 0.0f) target = fminf(target, snap_value(a, i + snap_max > last ? last : i + snap_max));
        else target = fmaxf(target, snap_value(a, i - snap_max < 0 ? 0 : i - snap_max));
    }
    target = clampf(target, 0.0f, a->max);
    float dist = target - a->pos;
    if (dist == 0.0f) return false;
    if (dur_us == 0.0f) {
        dur_us = 3.0f * fabsf(dist) / fabsf(v) * 1e6f;
        dur_us = clampf(dur_us, (float)SNAP_MIN_US, (float)SNAP_MAX_US);
    }
    a->mode = M_SNAP;
    a->a = a->pos; a->b = target; a->c = dur_us * 1e-6f;
    a->t0 = t;
    a->v = 0.0f;
    return true;
}

static void release_motion(slate_scroll_t *s, int64_t t_us, const float v[2])
{
    for (int i = 0; i < 2; i++) {
        slate_sc_axis_t *a = &s->ax[i];
        a->mode = M_NONE;
        a->v = 0.0f;
        if (a->pos < 0.0f) { start_spring(a, 0.0f, -1.0f, -a->pos, -v[i] * band_slope(-a->pos, a->view), t_us); continue; }
        if (a->pos > a->max) { start_spring(a, a->max, 1.0f, a->pos - a->max, v[i] * band_slope(a->pos - a->max, a->view), t_us); continue; }
        if (!axis_ok(s, i) || a->max <= 0.0f) continue;
        if (a->snap > 0) { start_snap(a, v[i], t_us, s->snap_max); continue; }
        if (fabsf(v[i]) >= STOP_V) { a->mode = M_FLING; a->v = v[i]; a->t0 = t_us; }
    }
}

void slate_scroll_release(slate_scroll_t *s, int64_t t_us)
{
    if (!s->armed) return;
    float v[2] = { 0.0f, 0.0f };
    if (s->dragging) {
        velocity(s, t_us, v);
        for (int i = 0; i < 2; i++) v[i] = axis_ok(s, i) ? clampf(v[i], -MAX_V, MAX_V) : 0.0f;
    } else {
        bool over = false, snapped = false;
        for (int i = 0; i < 2; i++) {
            over |= s->ax[i].pos < 0.0f || s->ax[i].pos > s->ax[i].max;
            snapped |= s->stopped && s->ax[i].snap > 0;
        }
        if (!over && !snapped) { s->armed = false; s->npoints = 0; s->head = 0; refresh(s); return; }
    }
    s->last_now = t_us;
    bool was_drag = s->dragging;
    s->armed = false;
    s->dragging = false;
    s->npoints = 0;
    s->head = 0;
    release_motion(s, t_us, v);
    refresh(s);
    if (was_drag && s->state == SLATE_SC_IDLE) s->ended = true;    /* a drag always ends: no motion follows */
}

void slate_scroll_cancel(slate_scroll_t *s, int64_t t_us)
{
    if (!s->armed) return;
    s->npoints = 0;                     /* no points: velocity 0 */
    s->head = 0;
    slate_scroll_release(s, t_us);
}

void slate_scroll_jump(slate_scroll_t *s, float x, float y)
{
    if (!isfinite(x) || !isfinite(y)) return;
    stop_all(s);
    s->ax[0].pos = clampf(x, 0.0f, s->ax[0].max);
    s->ax[1].pos = clampf(y, 0.0f, s->ax[1].max);
    refresh(s);
}

/* The spring's displacement and velocity (px, px/s) at t seconds after its start. */
static void spring_at(const slate_sc_axis_t *a, float t, float *x, float *v)
{
    float k = a->d + OMEGA * a->c, e = expf(-OMEGA * t);
    *x = (a->c + k * t) * e;
    *v = (k - OMEGA * (a->c + k * t)) * e;
}

static bool step_axis(slate_scroll_t *s, slate_sc_axis_t *a, int64_t now)
{
    if (a->mode == M_NONE) return false;

    /* The limits may have changed under a motion: put it back inside, never past half the view. */
    if (a->mode != M_SPRING && (a->pos < 0.0f || a->pos > a->max)) {
        float edge = a->pos < 0.0f ? 0.0f : a->max, sign = a->pos < 0.0f ? -1.0f : 1.0f;
        if (!s->bounce) { a->pos = edge; a->mode = M_NONE; a->v = 0.0f; return false; }
        start_spring(a, edge, sign, fabsf(a->pos - edge), a->mode == M_FLING ? sign * a->v : 0.0f, now);
    } else if (a->mode == M_SNAP) {
        float nb = clampf(a->b, 0.0f, a->max);
        if (nb != a->b) {
            float rem = a->c - (float)(now - a->t0) * 1e-6f;
            if (rem <= 0.0f || nb == a->pos) { a->pos = nb; a->mode = M_NONE; return false; }
            a->a = a->pos; a->b = nb; a->c = rem; a->t0 = now;
        }
    } else if (a->mode == M_SPRING && a->b > 0.0f && a->a != a->max) {
        float x, v;
        spring_at(a, (float)(now - a->t0) * 1e-6f, &x, &v);
        if (a->pos > a->max) start_spring(a, a->max, 1.0f, a->pos - a->max, v, now);
        else { a->mode = M_NONE; return false; }
    }

    if (a->mode == M_FLING) {
        float dt = (float)(now - a->t0) * 1e-6f;
        if (dt <= 0.0f) return true;
        float e = expf(-dt / TAU_S);
        float prev = a->pos, next = prev + a->v * TAU_S * (1.0f - e);
        if (next >= 0.0f && next <= a->max) {
            a->pos = next;
            a->v *= e;
            a->t0 = now;
            if (fabsf(a->v) < STOP_V) { a->mode = M_NONE; a->v = 0.0f; return false; }
            return true;
        }
        float edge = next < 0.0f ? 0.0f : a->max, sign = next < 0.0f ? -1.0f : 1.0f;
        if (!s->bounce) { a->pos = edge; a->mode = M_NONE; a->v = 0.0f; return false; }
        /* The exact crossing inside the step: inertia up to it, the spring for the rest of dt. */
        float dist = fabsf(edge - prev), reach = fabsf(a->v) * TAU_S;
        float tc = dist >= reach ? dt : -TAU_S * logf(1.0f - dist / reach);
        if (tc > dt) tc = dt;
        float vc = a->v * expf(-tc / TAU_S);
        start_spring(a, edge, sign, 0.0f, sign * vc, a->t0 + (int64_t)(tc * 1e6f));
        a->pos = edge;
    }
    if (a->mode == M_SNAP) {
        float u = (float)(now - a->t0) * 1e-6f / a->c;
        if (u >= 1.0f) { a->pos = a->b; a->mode = M_NONE; return false; }
        a->pos = a->a + (a->b - a->a) * slate_ease(SLATE_EASE_OUT_CUBIC, u);
        return true;
    }
    if (a->mode == M_SPRING) {
        float t = (float)(now - a->t0) * 1e-6f, x, v;
        if (t < 0.0f) t = 0.0f;
        spring_at(a, t, &x, &v);
        if (t > 0.0f && fabsf(x) < 0.5f && fabsf(v) < STOP_V) {
            a->pos = a->a;
            a->mode = M_NONE;
            return false;
        }
        float cap = a->view * 0.5f;
        a->pos = a->a + a->b * (x > cap ? cap : x);
        return true;
    }
    return false;
}

bool slate_scroll_step(slate_scroll_t *s, int64_t now_us)
{
    if (s->state != SLATE_SC_FLING && s->state != SLATE_SC_SNAP && s->state != SLATE_SC_SPRING) return false;
    if (now_us < s->last_now) now_us = s->last_now;       /* time never goes back */
    s->last_now = now_us;
    bool active = false;
    for (int i = 0; i < 2; i++) {
        slate_sc_axis_t *a = &s->ax[i];
        active |= step_axis(s, a, now_us);
        if (a->mode == M_NONE && (a->pos < 0.0f || a->pos > a->max)) {     /* never rest past an edge */
            if (s->bounce) {
                float edge = a->pos < 0.0f ? 0.0f : a->max;
                start_spring(a, edge, a->pos < 0.0f ? -1.0f : 1.0f, fabsf(a->pos - edge), 0.0f, now_us);
                active = true;
            } else {
                a->pos = clampf(a->pos, 0.0f, a->max);
            }
        }
    }
    if (!active) s->ended = true;
    refresh(s);
    return active;
}

bool slate_scroll_take_end(slate_scroll_t *s)
{
    bool e = s->ended;
    s->ended = false;
    return e;
}

void slate_scroll_pos(const slate_scroll_t *s, float *x, float *y)
{
    if (x) *x = s->ax[0].pos;
    if (y) *y = s->ax[1].pos;
}

void slate_scroll_max(const slate_scroll_t *s, float *mx, float *my)
{
    if (mx) *mx = s->ax[0].max;
    if (my) *my = s->ax[1].max;
}

slate_sc_state_t slate_scroll_state(const slate_scroll_t *s) { return s->state; }
