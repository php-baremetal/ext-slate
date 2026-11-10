/* beam.h - when a band may go to a panel without tearing ("racing the beam"). Pure C, no ESP-IDF and no PHP: the
   engine feeds it the TE pulses and the band timings it measures, and asks it when to push.

   The panel refreshes its picture from its own RAM, one line after the other in its scan order (display_scan_t of
   the band contract), once per TE period: the TE pulse (its falling edge, the end of the vertical blanking) starts
   the scan at the first line. A band written while the scan crosses it shows split: the lines already read keep
   the old picture for one refresh, the others show the new one. So a band of scan lines [a, b) goes either
   wholly ahead of the scan (its write ends before the scan reaches a) or wholly behind it (its write starts after
   the scan left b, and ends before the next refresh reaches a). The bands of a frame all go behind the scan of the
   refresh that the frame's pulse started, so they all show in the next one: a band that went ahead would show one
   refresh earlier than its neighbours, and whether it can go ahead depends on its row and on how long the bands
   before it took, so an element moving across the screen would show some frames early and skip or repeat a step.

   The model, deliberately on the safe side:
   - the period: the TE intervals, filtered (SLATE_BEAM_PERIOD_MIN_US .. _MAX_US accepted; the model is usable after
     SLATE_BEAM_LOCK intervals in a row within 1/8 of the filtered period, and while the last pulse is younger
     than SLATE_BEAM_STALE periods);
   - the scan reaches line s between s/lines * P * SLATE_BEAM_ACTIVE_MIN_PCT / 100 and s/lines * P after the pulse
     (the blanking takes a part of the period: measured 4 % on an ST77916), give or take SLATE_BEAM_MARGIN_US;
   - only frames that change at most SLATE_BEAM_AREA_MAX_PCT of the screen race the scan. A larger frame takes about
     a refresh or more to send, so a refresh shows it partly drawn whatever the order (the edge between the bands
     sent and those still to come): holding its bands back would only make it slower;
   - a band's write lasts from its push until its done at most: fixed_us + bytes * ns_per_byte / 1000, measured on
     the bands already sent (slate_xfer_sample), the fixed part kept near the slowest bands. A band pushed while
     others are in flight starts when the driver gets to it (ready_us, an upper estimate). */
#ifndef SLATE_BEAM_H
#define SLATE_BEAM_H

#include <stdbool.h>
#include <stdint.h>

#include "display_band.h"

#ifndef SLATE_BEAM_MARGIN_US
#define SLATE_BEAM_MARGIN_US 150        /* TE interrupt latency and period jitter */
#endif
#ifndef SLATE_BEAM_ACTIVE_MIN_PCT
#define SLATE_BEAM_ACTIVE_MIN_PCT 90    /* the scan of a refresh lasts at least this share of the period */
#endif
#define SLATE_BEAM_PERIOD_MIN_US 4000   /* 250 Hz */
#define SLATE_BEAM_PERIOD_MAX_US 100000 /* 10 Hz */
#define SLATE_BEAM_LOCK 4
#define SLATE_BEAM_STALE 4
#ifndef SLATE_BEAM_AREA_MAX_PCT
#define SLATE_BEAM_AREA_MAX_PCT 50      /* a frame races the scan when it changes at most this share of the screen */
#endif

/* The TE pulses. Written by the TE interrupt only (slate_beam_pulse), read by the engine task: 32-bit fields (the
   time is the low 32 bits of the microsecond clock, compared modulo 2^32), each one atomic. */
typedef struct {
    uint32_t last_us;       /* the last pulse */
    uint32_t period_us;     /* filtered period, 0 = none yet */
    uint32_t good;          /* intervals in a row that matched the period */
} slate_te_t;

/* Band timings. Engine task only. */
typedef struct {
    uint32_t fixed_us;      /* per band */
    uint32_t ns_per_byte;
} slate_xfer_t;

/* A pulse at now_us (ISR-safe: plain arithmetic and atomic stores). */
void slate_beam_pulse(slate_te_t *te, int64_t now_us);
/* The model at now_us, false when not usable (no TE, too few or irregular pulses, the last one too old). */
bool slate_beam_model(const slate_te_t *te, int64_t now_us, int64_t *te_us, uint32_t *period_us);

void slate_xfer_init(slate_xfer_t *x);
/* A band of bytes took us from the moment the driver could start it to its done. */
void slate_xfer_sample(slate_xfer_t *x, uint32_t bytes, uint32_t us);
/* How long a band of bytes may take, at most. */
uint32_t slate_xfer_us(const slate_xfer_t *x, uint32_t bytes);

/* The scan lines [*a, *b) of *lines that a band of panel rows [y0, y0 + rows) of the region [x0, x1) covers, for a
   w x h panel; false for DISPLAY_SCAN_UNKNOWN (or an unknown value). */
bool slate_beam_span(display_scan_t scan, int w, int h, int x0, int x1, int y0, int rows, int *a, int *b,
                     int *lines);
/* The earliest time >= now_us at which a band of scan lines [a, b) of lines may be pushed: its write, from the push
   (or from ready_us, when the driver is busy until then) for d_us, must not meet the scan of any refresh, and it
   starts only once the scan of the refresh that began at frame_te_us (the pulse that started the frame) has left the
   band. So no band of a frame goes ahead of that scan: every band shows in the next refresh (or a later one when the
   frame runs late), whatever its place on the screen and whatever the bands before it waited, and a moving element
   keeps the same delay from frame to frame. now_us when it is safe now, or when no time is (the band is longer than
   a refresh allows: it goes at once). te_us is a pulse at or before now_us. */
int64_t slate_beam_when(int64_t te_us, uint32_t period_us, int a, int b, int lines, int64_t now_us, int64_t ready_us,
                        uint32_t d_us, int64_t frame_te_us);

#endif
