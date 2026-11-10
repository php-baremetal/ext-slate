/* engine.h - Slate's compositor task: owns the scene, applies the PHP task's commands, composes the frames and
   sends them to the panel through ext-display's band path. Pure C, no ESP-IDF and no PHP: the task, queues,
   locks and memory come from the events HAL of ext-events, the panel from display_band.h of ext-display.

   Threads.
     PHP task       slate_engine_start / stop / cmds / deliver / kick / stats / set_cache_budget
     TE interrupt   slate_engine_vsync (give it to display_vsync_hook(): ISR-safe; notes the pulse, wakes the task)
     engine task    everything else: the scene, the compositor, every call of the band ops (buf_alloc, begin,
                    push, end, buf_free), the debug output
     driver task    the band ops' done(arg): marks a band buffer free and wakes a wait for it
   The engine task sleeps until a kick (a delivery), a TE pulse it asked for, or a pacing deadline. Awake, it
   applies every delivered batch, in order, before anything else (a frame never shows half a batch); then, when
   something is dirty, it composes one frame:
     - with TE (has_te): at the first pulse after the change; at most one frame per pulse. The interrupt wakes the
       task only while something is dirty (a still screen costs no wake-up at all). When no pulse comes for
       SLATE_TE_FALLBACK_MS while something is dirty (another display object took the TE pin, the panel sleeps),
       the frame goes anyway (counted in te_fallbacks): a lost TE source slows Slate down, never freezes it;
     - without TE: at once, but at most one frame every 1000 / refresh_hz ms (16 ms when refresh_hz is 0).
   With TE and a driver that gives its refresh order (display_band_ops_t::scan), a frame that changes at most
   SLATE_BEAM_AREA_MAX_PCT of the screen races the panel's scan (beam.h): before each push the band waits, when needed,
   until the panel's refresh is out of its way (on the HAL timer, or the queue's clock; a done ends the sleep early),
   so it is written wholly behind the refresh and does not tear. Every band of the frame goes behind the refresh that
   the frame's pulse started, so the whole frame shows in the next one (the same delay for every row: a moving element
   does not skip a step where its band could have gone ahead). The wait counts in wait_us.
   A frame goes region by region (band_begin, the region's bands through band_push, band_end), two band buffers
   in ping-pong: the compositor draws one while the panel sends the other; a buffer is written again only after
   the done of its last push. A frame aborted by the compositor keeps its dirty set and is retried at the next
   pulse (or pacing deadline). A band the driver refuses (push error) is marked dirty again and sent by the next
   frames (3 frames in a row at most, so a dead panel does not keep the task busy; see Panel failure).

   Panel failure. A wait for a band's done lasts SLATE_DONE_TIMEOUT_MS at most. Past it the engine fails for good:
   slate_engine_error() gives the reason, nothing more is sent to the panel (the commands are still applied), and
   that buffer is never pushed or freed again (the driver may still read it: it is leaked). It also fails, with
   "the panel refused the bands (error 0x..)" and no buffer lost, when begin or push returns BM_ERR_INVALID_STATE (a panel that failed
   for good, display_band.h) or when SLATE_REFUSED_FRAMES frames in a row had every push refused.
   slate_engine_stop() always returns, within SLATE_STOP_TIMEOUT_MS.

   Elements. The PHP task names them by ids (uint32, not 0) that it assigns; the engine maps ids to its elements
   (an open-addressing table in external RAM). Commands for an id that does not exist (or whose NEW failed for
   lack of memory) are ignored and counted in cmd_errors, like refused property values. A FREE of a group
   detaches its children, which stay alive under their own ids. The shown scene (SHOW) is drawn under an
   internal screen root; with the slate-debug option the FPS counter sits on top of it.

   Memory. Elements, texts, gradients, canvas pixels, element caches, the id table and the command buffer:
   external RAM (BM_HAL_MEM_EXTERNAL, PSRAM on the board). The two band buffers: the driver's buf_alloc,
   rows * w * 2 bytes with rows the driver's advice cut to bytes_max, taken while a block of the whole scratch
   (3 * w * rows) is held and then freed, so that the bands go where internal RAM is in smaller pieces and a piece
   for the scratch stays (the heap's regions do not join); when buf_alloc refuses, the same rows again without that
   block, then the rows are halved down to 8 (the start fails below). The compositor's scratch arena: internal RAM,
   none at start; taken when the
   compositor asks for it (compose.h, scratch_grow: what a group composed offscreen or a cache build wants, at most
   3 * w * rows bytes), a new block that replaces the old one, or the largest of half, a quarter, ... of the size
   asked for that fits (a size refused is not asked for again within a second); shrunk to the most the frames asked
   for when none asked for all of it for a second (also without frames: the task wakes for it), freed when that
   was nothing and when another scene is shown. So internal RAM that a scene does not use stays free for the rest
   of the program (Wi-Fi, DMA). Element caches stop growing at a
   budget (cache_budget, default: half of the largest external block found at start, at most 2 MB): past it the
   compositor draws those elements directly (counted as cache misses). Caches only ever take external RAM
   (BM_HAL_MEM_EXTERNAL_ONLY): on a board without PSRAM the budget is 0, nothing is cached and every element is
   drawn directly, so internal RAM is left to the bands, Wi-Fi and DMA.

   One engine at a time (the compositor and the rasterizers are not reentrant). */
#ifndef SLATE_ENGINE_H
#define SLATE_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cmd.h"
#include "compose.h"
#include "display_band.h"

#ifndef SLATE_ENGINE_STACK
#define SLATE_ENGINE_STACK 8192        /* bytes */
#endif
#ifndef SLATE_ENGINE_PRIORITY
#define SLATE_ENGINE_PRIORITY 0        /* relative to the PHP task */
#endif
#ifndef SLATE_ENGINE_CORE
#define SLATE_ENGINE_CORE BM_HAL_CORE_0
#endif
#define SLATE_CACHE_BUDGET_MAX (2u * 1024 * 1024)
#ifndef SLATE_DONE_TIMEOUT_MS
#define SLATE_DONE_TIMEOUT_MS 2000     /* longest wait for a band's done (as ext-display's flush deadline) */
#endif
#ifndef SLATE_TE_FALLBACK_MS
#define SLATE_TE_FALLBACK_MS 250       /* with TE: frame anyway when no pulse came for this long */
#endif
#define SLATE_REFUSED_FRAMES 4         /* frames in a row with every push refused: the panel is taken as failed */
/* longest wait of slate_engine_stop for the task: a frame in progress and the drain of both buffers */
#define SLATE_STOP_TIMEOUT_MS (3 * SLATE_DONE_TIMEOUT_MS + 1000)

typedef struct slate_engine slate_engine_t;

typedef struct {
    float fps;                   /* frames per second of the scene: the intervals between its frames, smoothed
                                    (0 when its last frame is older than a second; FPS-counter-only frames are
                                    left out) */
    uint32_t frames;             /* frames composed since the start (aborted ones not counted) */
    uint32_t aborted;            /* frames the compositor aborted (retried later) */
    uint32_t batches, cmds;      /* batches and commands applied */
    uint32_t cmd_errors;         /* commands ignored: unknown id, refused value, out of memory */
    uint32_t push_errors;        /* band ops that returned an error (the band is sent again by the next frame) */
    uint32_t te_fallbacks;       /* frames started without a pulse (with TE, none for SLATE_TE_FALLBACK_MS) */
    uint32_t beam_waits;         /* bands held back until the panel's scan was out of their way (with TE) */
    uint32_t elements;           /* elements alive (the ids mapped) */
    uint32_t frame_us;           /* the last frame: from its start to its last band handed over */
    uint32_t compose_us;         /*   of which drawing */
    uint32_t wait_us;            /*   of which waiting for the panel (free buffers, accepted pushes) */
    slate_stats_t last;          /* the compositor's stats of the last frame */
    size_t cache_bytes;          /* element cache memory in use */
    size_t cache_budget;
    int band_rows;               /* rows per band */
    size_t scratch_bytes;        /* scratch arena now (0: none) */
} slate_engine_stats_t;

/* Starts the engine task for a panel of w x h pixels with the band path ops (ops->begin etc.; panel is the
   driver's pointer, passed back to every op). refresh_hz: the panel's refresh rate (0 = unknown); has_te: the
   caller hooked slate_engine_vsync to the TE pulses (display_vsync_hook()). Returns once the task has its band
   buffers; NULL when an argument is wrong, an engine already runs, or memory, a queue or the task
   cannot be had. PHP task only. */
slate_engine_t *slate_engine_start(const display_band_ops_t *ops, void *panel, int w, int h, float refresh_hz,
                                   bool has_te);
/* Stops it: the task finishes the frame in progress, waits for the done of every band in flight (each at most
   SLATE_DONE_TIMEOUT_MS), frees the scene (every element still mapped), the caches, its band buffers (buf_free)
   and scratch, and ends; then the engine and its command buffer are freed. Delivered batches not applied yet and
   the pending one are dropped. Remove the vsync hook first (display_vsync_hook(obj, NULL, NULL)). Returns within
   SLATE_STOP_TIMEOUT_MS: true when everything was freed; false when the panel lost a band (that buffer and the
   engine's small struct stay allocated, as a late done may still use them) or the task did not end in time (it
   is stuck in the driver: everything stays, and no engine can start again). NULL allowed (true). PHP task only. */
bool slate_engine_stop(slate_engine_t *e);
/* NULL while the engine works; else why it stopped sending to the panel, a short sentence for a message (e.g.
   "the panel did not finish a band within 2000 ms"). Sticky. Any task. */
const char *slate_engine_error(slate_engine_t *e);

/* The command buffer the PHP task pushes into (slate_cmd_push). */
slate_cmdbuf_t *slate_engine_cmds(slate_engine_t *e);
/* Delivers the pending batch (slate_cmd_deliver) and wakes the task; false when there was nothing to deliver. */
bool slate_engine_deliver(slate_engine_t *e);
/* Wakes the task (a delivery is waiting). PHP task (any task, in fact; not an ISR). */
void slate_engine_kick(slate_engine_t *e);
/* A TE pulse: void(void *) for display_vsync_hook(), arg = the engine. ISR-safe (an atomic and
   bm_hal_queue_send_isr()); notes the pulse's time (the scan's period and phase), and wakes nothing while nothing is
   dirty. */
void slate_engine_vsync(void *engine);
/* The panel goes to sleep (true) or is awake again (false): while asleep no frame goes (the commands are still
   applied, the changes wait), so nothing is sent to a panel that is off and no TE fallback frame runs. Any task
   (not an ISR). */
void slate_engine_pause(slate_engine_t *e, bool asleep);
/* The panel lost its picture (a re-initialisation on wake): the next frame sends the whole screen. Nothing before
   the first frame (nothing was drawn yet). Any task (not an ISR). */
void slate_engine_redraw(slate_engine_t *e);
/* A copy of the statistics. Any task. */
void slate_engine_stats(slate_engine_t *e, slate_engine_stats_t *out);
/* Caps the element cache memory at bytes (taken at the next allocation; what is already cached stays).
   Any task. */
void slate_engine_set_cache_budget(slate_engine_t *e, size_t bytes);

#endif
