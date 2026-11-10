/* Racing the panel's scan: see beam.h. */
#include "beam.h"

#define XFER_RATE_BYTES 4096        /* bands from this size on tell the rate (the fixed part is small next to it) */
#define XFER_FIXED_INIT 1000
#define XFER_NSPB_INIT 100          /* 10 bytes a microsecond */
#define XFER_FIXED_MAX 100000
#define XFER_NSPB_MAX 100000

static uint32_t ld(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static void st(uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
}

void slate_beam_pulse(slate_te_t *te, int64_t now_us)
{
    uint32_t t = (uint32_t)now_us, dt = t - ld(&te->last_us), per = ld(&te->period_us), good = ld(&te->good);
    st(&te->last_us, t);
    if (dt < SLATE_BEAM_PERIOD_MIN_US || dt > SLATE_BEAM_PERIOD_MAX_US) {
        st(&te->good, 0);   /* the first pulse, or one after a pause: the period is measured again */
        return;
    }
    uint32_t dev = dt > per ? dt - per : per - dt;
    if (per == 0 || dev > per / 8) {
        per = dt;           /* another rate, or a pulse missed: start again from this interval */
        good = 0;
    } else {
        per = (uint32_t)((int32_t)per + ((int32_t)dt - (int32_t)per) / 8);
        good = good < SLATE_BEAM_LOCK ? good + 1 : good;
    }
    st(&te->period_us, per);
    st(&te->good, good);
}

bool slate_beam_model(const slate_te_t *te, int64_t now_us, int64_t *te_us, uint32_t *period_us)
{
    uint32_t per = ld(&te->period_us), good = ld(&te->good), last = ld(&te->last_us);
    if (per == 0 || good < SLATE_BEAM_LOCK) {
        return false;
    }
    uint32_t age = (uint32_t)now_us - last;
    if (age > per * SLATE_BEAM_STALE) {
        return false;   /* the pulses stopped (another object took the TE pin, the panel sleeps) */
    }
    *te_us = now_us - age;
    *period_us = per;
    return true;
}

void slate_xfer_init(slate_xfer_t *x)
{
    x->fixed_us = XFER_FIXED_INIT;
    x->ns_per_byte = XFER_NSPB_INIT;
}

void slate_xfer_sample(slate_xfer_t *x, uint32_t bytes, uint32_t us)
{
    if (bytes >= XFER_RATE_BYTES) {
        uint32_t data = us > x->fixed_us ? us - x->fixed_us : 0;
        uint32_t nspb = (uint32_t)((uint64_t)data * 1000 / bytes);
        int32_t n = (int32_t)x->ns_per_byte + ((int32_t)nspb - (int32_t)x->ns_per_byte) / 8;
        x->ns_per_byte = n < 1 ? 1 : n > XFER_NSPB_MAX ? XFER_NSPB_MAX : (uint32_t)n;
    }
    uint32_t data = (uint32_t)((uint64_t)bytes * x->ns_per_byte / 1000);
    uint32_t fixed = us > data ? us - data : 0;
    if (fixed > x->fixed_us) {
        x->fixed_us += (fixed - x->fixed_us + 1) / 2;   /* up fast: the slow bands set it */
    } else {
        x->fixed_us -= (x->fixed_us - fixed) / 32;      /* down slowly */
    }
    if (x->fixed_us > XFER_FIXED_MAX) {
        x->fixed_us = XFER_FIXED_MAX;
    }
}

uint32_t slate_xfer_us(const slate_xfer_t *x, uint32_t bytes)
{
    return x->fixed_us + (uint32_t)((uint64_t)bytes * x->ns_per_byte / 1000);
}

bool slate_beam_span(display_scan_t scan, int w, int h, int x0, int x1, int y0, int rows, int *a, int *b,
                     int *lines)
{
    switch (scan) {
    case DISPLAY_SCAN_TOP_DOWN:
        *a = y0, *b = y0 + rows, *lines = h;
        return true;
    case DISPLAY_SCAN_BOTTOM_UP:
        *a = h - (y0 + rows), *b = h - y0, *lines = h;
        return true;
    case DISPLAY_SCAN_LEFT_RIGHT:
        *a = x0, *b = x1, *lines = w;
        return true;
    case DISPLAY_SCAN_RIGHT_LEFT:
        *a = w - x1, *b = w - x0, *lines = w;
        return true;
    default:
        return false;
    }
}

int64_t slate_beam_when(int64_t te_us, uint32_t period_us, int a, int b, int lines, int64_t now_us, int64_t ready_us,
                        uint32_t d_us, int64_t frame_te_us)
{
    int64_t p = period_us;
    if (p <= 0 || lines <= 0 || a >= b) {
        return now_us;
    }
    /* the scan is inside the band from enter to leave after a pulse (the earliest entry, the latest exit) */
    int64_t enter = (int64_t)a * p * SLATE_BEAM_ACTIVE_MIN_PCT / (100 * (int64_t)lines) - SLATE_BEAM_MARGIN_US;
    int64_t leave = (int64_t)b * p / lines + SLATE_BEAM_MARGIN_US;
    if (leave - enter + (int64_t)d_us >= p) {
        return now_us;   /* no gap between two crossings holds the write */
    }
    /* never ahead of the scan of the frame's own refresh: every band of a frame shows in the same refresh */
    int64_t t = frame_te_us + leave > now_us ? frame_te_us + leave + 1 : now_us;
    for (int tries = 0; tries < 8; tries++) {
        int64_t end = (ready_us > t ? ready_us : t) + d_us;
        if (end - t + (leave - enter) >= p) {
            return now_us;   /* the driver is busy for too long: waiting would not help */
        }
        int64_t r = t - te_us;
        int64_t k = (r >= 0 ? r / p : -((-r + p - 1) / p)) - 1;
        bool hit = false;
        for (int64_t j = k; j <= k + 3; j++) {
            int64_t f0 = te_us + j * p + enter, f1 = te_us + j * p + leave;
            if (t <= f1 && end >= f0) {
                t = f1 + 1;   /* behind the scan, as soon as it left the band */
                hit = true;
                break;
            }
        }
        if (!hit) {
            return t;
        }
    }
    return now_us;
}
