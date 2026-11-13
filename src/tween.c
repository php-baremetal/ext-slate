/* tween.c - Slate's easing curves and tweens (see tween.h). Pure C, no ESP-IDF and no PHP. */
#include "tween.h"

#include <math.h>
#include <string.h>

#define PROPS_MAX 255
#define NOT_PAUSED INT64_MIN

#define F_STARTED 0x01u
#define F_DONE 0x02u
#define F_YOYO 0x04u
#define F_START_NEXT_SAMPLE 0x08u   /* chained after a tween already done: starts at its first sample */

struct slate_tween {
    int64_t start_us;         /* the start (before the delay), moved by every pause */
    int64_t paused_at;        /* NOT_PAUSED, or the time of the pause */
    slate_tween_t *next;
    int32_t ms;
    int32_t delay_ms;
    int32_t repeat;           /* -1: always */
    uint8_t ease;
    uint8_t n;
    uint8_t flags;
    slate_tween_prop_t props[];
};

/* ---- curves ---- */

static float out_bounce(float t)
{
    const float n1 = 7.5625f, d1 = 2.75f;
    if (t < 1.0f / d1) {
        return n1 * t * t;
    }
    if (t < 2.0f / d1) {
        t -= 1.5f / d1;
        return n1 * t * t + 0.75f;
    }
    if (t < 2.5f / d1) {
        t -= 2.25f / d1;
        return n1 * t * t + 0.9375f;
    }
    t -= 2.625f / d1;
    return n1 * t * t + 0.984375f;
}

float slate_ease(slate_ease_t e, float t)
{
    if (!(t > 0.0f)) {
        return 0.0f;
    }
    if (t >= 1.0f) {
        return 1.0f;
    }
    float u = 1.0f - t;
    switch (e) {
    case SLATE_EASE_IN_QUAD:
        return t * t;
    case SLATE_EASE_OUT_QUAD:
        return 1.0f - u * u;
    case SLATE_EASE_IN_OUT_QUAD:
        return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * u * u;
    case SLATE_EASE_IN_CUBIC:
        return t * t * t;
    case SLATE_EASE_OUT_CUBIC:
        return 1.0f - u * u * u;
    case SLATE_EASE_IN_OUT_CUBIC:
        return t < 0.5f ? 4.0f * t * t * t : 1.0f - 4.0f * u * u * u;
    case SLATE_EASE_OUT_BACK: {
        const float c1 = 1.70158f, c3 = c1 + 1.0f;
        float v = t - 1.0f;
        return 1.0f + c3 * v * v * v + c1 * v * v;
    }
    case SLATE_EASE_OUT_BOUNCE:
        return out_bounce(t);
    case SLATE_EASE_OUT_ELASTIC: {
        const float c4 = 2.0f * 3.14159265358979f / 3.0f;
        return exp2f(-10.0f * t) * sinf((t * 10.0f - 0.75f) * c4) + 1.0f;
    }
    case SLATE_EASE_LINEAR:
    default:
        return t;
    }
}

/* ---- records ---- */

size_t slate_tween_size(int n)
{
    return sizeof(slate_tween_t) + (size_t)(n > 0 ? n : 0) * sizeof(slate_tween_prop_t);
}

static slate_tween_t *make(const slate_tween_prop_t *props, int n, int ms, slate_ease_t ease, int delay_ms,
                           int repeat, bool yoyo)
{
    if (n < 0 || n > PROPS_MAX || (n > 0 && !props) || ms < 1 || delay_ms < 0 || repeat < -1 ||
        (unsigned)ease >= SLATE_EASE_COUNT) {
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        if (props[i].type != SLATE_VAL_NUM && props[i].type != SLATE_VAL_COLOR) {
            return NULL;
        }
    }
    slate_tween_t *t = slate_scene_mem(slate_tween_size(n));
    if (!t) {
        return NULL;
    }
    t->paused_at = NOT_PAUSED;
    t->ms = ms;
    t->delay_ms = delay_ms;
    t->repeat = repeat;
    t->ease = (uint8_t)ease;
    t->n = (uint8_t)n;
    t->flags = yoyo ? F_YOYO : 0;
    if (n > 0) {
        memcpy(t->props, props, (size_t)n * sizeof *props);
    }
    return t;
}

slate_tween_t *slate_tween_new(const slate_tween_prop_t *props, int n, int64_t start_us, int ms, slate_ease_t ease,
                               int delay_ms, int repeat, bool yoyo)
{
    slate_tween_t *t = make(props, n, ms, ease, delay_ms, repeat, yoyo);
    if (t) {
        t->start_us = start_us;
        t->flags |= F_STARTED;
    }
    return t;
}

slate_tween_t *slate_tween_new_chained(const slate_tween_prop_t *props, int n, int ms, slate_ease_t ease,
                                       int delay_ms, int repeat, bool yoyo)
{
    return make(props, n, ms, ease, delay_ms, repeat, yoyo);
}

void slate_tween_free(slate_tween_t *t)
{
    slate_scene_mem_free(t);
}

/* ---- time ---- */

/* The time from the start to the end (delay included), INT64_MAX when it never ends (or does not fit). */
static int64_t span_us(const slate_tween_t *t)
{
    if (t->repeat < 0) {
        return INT64_MAX;
    }
    int64_t run = (int64_t)t->ms * 1000;
    int64_t runs = (int64_t)t->repeat + 1;
    if (runs > (INT64_MAX - (int64_t)t->delay_ms * 1000) / run) {
        return INT64_MAX;
    }
    return (int64_t)t->delay_ms * 1000 + runs * run;
}

static int64_t end_of(const slate_tween_t *t)
{
    int64_t span = span_us(t);
    if (span == INT64_MAX || t->start_us > INT64_MAX - span) {
        return INT64_MAX;
    }
    return t->start_us + span;
}

int64_t slate_tween_end_us(const slate_tween_t *t)
{
    if (!(t->flags & F_STARTED) || t->paused_at != NOT_PAUSED) {
        return INT64_MAX;
    }
    return end_of(t);
}

static void start_at(slate_tween_t *t, int64_t start_us)
{
    t->start_us = start_us;
    t->flags |= F_STARTED;
    if (t->paused_at != NOT_PAUSED && t->paused_at < start_us) {
        t->paused_at = start_us;   /* paused before it started: the pause counts from the start */
    }
}

/* ---- values ---- */

static uint32_t lerp_argb(uint32_t from, uint32_t to, float e)
{
    uint32_t c = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int a = (int)((from >> sh) & 0xFFu), b = (int)((to >> sh) & 0xFFu);
        long v = a + lroundf((float)(b - a) * e);   /* halves away from zero: towards the target */
        if (v < 0) {
            v = 0;
        } else if (v > 255) {
            v = 255;
        }
        c |= (uint32_t)v << sh;
    }
    return c;
}

/* Writes the values of t at eased position e into out. */
static void write_values(const slate_tween_t *t, float e, slate_tween_prop_t *out)
{
    for (int i = 0; i < t->n; i++) {
        const slate_tween_prop_t *p = &t->props[i];
        out[i] = *p;
        if (p->type == SLATE_VAL_COLOR) {
            out[i].to.argb = e == 0.0f ? p->from.argb : e == 1.0f ? p->to.argb : lerp_argb(p->from.argb, p->to.argb, e);
        } else {
            out[i].to.num = e == 0.0f   ? p->from.num
                            : e == 1.0f ? p->to.num
                                        : p->from.num + (p->to.num - p->from.num) * e;
        }
    }
}

bool slate_tween_sample(slate_tween_t *t, int64_t now_us, slate_tween_prop_t *out, int *n)
{
    if (!(t->flags & F_STARTED)) {
        if (!(t->flags & F_START_NEXT_SAMPLE)) {
            *n = 0;
            return false;
        }
        t->flags &= (uint8_t)~F_START_NEXT_SAMPLE;
        start_at(t, now_us);
    }
    *n = t->n;
    int64_t now = t->paused_at != NOT_PAUSED ? t->paused_at : now_us;
    int64_t span = span_us(t);
    int64_t elapsed = now > t->start_us ? now - t->start_us : 0;   /* now >= start: no overflow for real clocks */
    bool yoyo = (t->flags & F_YOYO) != 0;

    if (span != INT64_MAX && elapsed >= span) {
        /* Done: the final values, exactly. */
        write_values(t, yoyo && (t->repeat & 1) ? 0.0f : 1.0f, out);
        if (!(t->flags & F_DONE)) {
            t->flags |= F_DONE;
            if (t->next && !(t->next->flags & F_STARTED)) {
                start_at(t->next, end_of(t));
            }
        }
        return true;
    }

    int64_t run = (int64_t)t->ms * 1000;
    int64_t in = elapsed - (int64_t)t->delay_ms * 1000;
    float p = 0.0f;
    bool back = false;
    if (in > 0) {
        int64_t k = in / run;
        /* In double (exact for any run that fits), then kept below 1: a long run never reaches its end value
           early. */
        p = (float)((double)(in % run) / (double)run);
        if (p >= 1.0f) {
            p = nextafterf(1.0f, 0.0f);
        }
        back = yoyo && (k & 1);
    }
    write_values(t, slate_ease((slate_ease_t)t->ease, back ? 1.0f - p : p), out);
    return false;
}

/* ---- pause, properties, chain ---- */

void slate_tween_pause(slate_tween_t *t, int64_t now_us)
{
    if (t->paused_at == NOT_PAUSED && !(t->flags & F_DONE)) {
        t->paused_at = (t->flags & F_STARTED) && now_us < t->start_us ? t->start_us : now_us;
    }
}

void slate_tween_resume(slate_tween_t *t, int64_t now_us)
{
    if (t->paused_at == NOT_PAUSED || (t->flags & F_DONE)) {
        return;
    }
    if ((t->flags & F_STARTED) && now_us > t->paused_at) {
        t->start_us += now_us - t->paused_at;
    }
    t->paused_at = NOT_PAUSED;
}

bool slate_tween_paused(const slate_tween_t *t)
{
    return t->paused_at != NOT_PAUSED;
}

bool slate_tween_drop_prop(slate_tween_t *t, slate_prop_t prop)
{
    for (int i = 0; i < t->n; i++) {
        if (t->props[i].prop == prop) {
            memmove(&t->props[i], &t->props[i + 1], (size_t)(t->n - i - 1) * sizeof t->props[0]);
            t->n--;
            return true;
        }
    }
    return false;
}

bool slate_tween_has_prop(const slate_tween_t *t, slate_prop_t prop)
{
    for (int i = 0; i < t->n; i++) {
        if (t->props[i].prop == prop) {
            return true;
        }
    }
    return false;
}

int slate_tween_count(const slate_tween_t *t)
{
    return t->n;
}

bool slate_tween_set_next(slate_tween_t *t, slate_tween_t *next)
{
    if (!next) {
        t->next = NULL;
        return true;
    }
    if (next->flags & (F_STARTED | F_START_NEXT_SAMPLE)) {
        return false;
    }
    slate_tween_t *last = t;
    for (;;) {
        if (last == next) {
            return false;   /* already in the chain */
        }
        if (!last->next) {
            break;
        }
        last = last->next;
    }
    for (const slate_tween_t *c = next->next; c; c = c->next) {
        if (c == last) {
            return false;   /* would make a loop (last is reached from next: also through a tail shared with t) */
        }
    }
    last->next = next;
    if (last->flags & F_DONE) {
        /* Its end is past (a sample saw it): next starts at its own first sample. */
        next->flags |= F_START_NEXT_SAMPLE;
    }
    return true;
}

slate_tween_t *slate_tween_next(const slate_tween_t *t)
{
    return t->next;
}

bool slate_tween_started(const slate_tween_t *t)
{
    return (t->flags & F_STARTED) != 0;
}

bool slate_tween_done(const slate_tween_t *t)
{
    return (t->flags & F_DONE) != 0;
}

bool slate_tween_active(const slate_tween_t *t)
{
    return (t->flags & (F_STARTED | F_START_NEXT_SAMPLE)) && !(t->flags & F_DONE) && t->paused_at == NOT_PAUSED;
}

bool slate_tween_endless(const slate_tween_t *t)
{
    return t->repeat < 0;
}

slate_tween_prop_t *slate_tween_props(slate_tween_t *t)
{
    return t->props;
}

void slate_tween_final(const slate_tween_t *t, slate_tween_prop_t *out, int *n)
{
    write_values(t, (t->flags & F_YOYO) && t->repeat > 0 && (t->repeat & 1) ? 0.0f : 1.0f, out);
    *n = t->n;
}

/* ---- writing into an element ---- */

static slate_value_t value_of(const slate_tween_prop_t *p)
{
    if (p->type != SLATE_VAL_COLOR) {
        return slate_v_num(p->to.num);
    }
    if (p->prop == SLATE_PROP_BORDER_COLOR) {
        return slate_v_color(p->to.argb);
    }
    return slate_v_paint((slate_paint_t){ p->to.argb, NULL });
}

void slate_tween_write(slate_el_t *e, const slate_tween_prop_t *vals, int n, slate_dirty_t *d, int sw, int sh)
{
    int ix = -1, iy = -1;
    for (int i = 0; i < n; i++) {
        if (vals[i].prop == SLATE_PROP_X && vals[i].type == SLATE_VAL_NUM) {
            ix = i;
        } else if (vals[i].prop == SLATE_PROP_Y && vals[i].type == SLATE_VAL_NUM) {
            iy = i;
        }
    }
    if (ix >= 0 && iy >= 0) {
        slate_value_t x = value_of(&vals[ix]), y = value_of(&vals[iy]);
        bool ok_x, ok_y;
        slate_el_move(e, &x, &y, d, sw, sh, &ok_x, &ok_y);
    }
    for (int i = 0; i < n; i++) {
        if (ix >= 0 && iy >= 0 && (i == ix || i == iy)) {
            continue;
        }
        slate_value_t v = value_of(&vals[i]);
        slate_el_set(e, vals[i].prop, &v, d, sw, sh);
    }
}
