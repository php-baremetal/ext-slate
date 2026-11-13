/* engine.h - Slate's compositor task: owns the scene, applies the PHP task's commands, composes the frames and
   sends them to the panel through ext-display's band path. Pure C, no ESP-IDF and no PHP: the task, queues,
   locks and memory come from the events HAL of ext-events, the panel from display_band.h of ext-display.

   Threads.
     PHP task       slate_engine_start / stop / cmds / deliver / kick / stats / set_cache_budget / set_keep / tween_done /
                    set_tween_listener / transition_done / in_transition / scroll_done (tween_done, transition_done
                    and scroll_done also from the tween listener); the slate_scroll_pub_* reads (cmd.h), any task
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

   Images. An IMAGE element shows its image from the first wake-up after the image is READY (images.h): at every
   wake-up, after the batches, when slate_images_done() changed the task syncs its IMAGE elements
   (slate_el_image_sync: the box of each one that shows its image now is dirty, the next frame draws it). The image
   store's listener (set by the PHP layer) kicks the task (slate_engine_kick) when an image is done. Image memory
   is not part of the cache budget.

   Tweens (tween.h). TWEEN, TWEEN_STOP and EASE commands (cmd.h) make, stop and set them up; the tweens live on the
   engine task only. A tween starts when its command is applied (its from values are the element's values then,
   slate_el_anim_get), or, chained after another (`after`), at that one's exact end, from the values it left. At every
   wake-up that gives a frame, after the batches and before the frame is composed, every running tween is sampled at
   the frame's time: the time of the TE pulse that starts the frame (without TE, or at a TE fallback: the time the
   frame starts), never the time spent computing, so a frame skipped leaves an animation on time. The values are
   written with slate_el_set (slate_tween_write): dirty areas and caches as for a change from PHP. While a tween runs
   the task asks for every pulse (without TE: a frame every period); without tweens or changes nothing goes (a
   still screen gives no frame), and a scene without tweens costs no work per frame. Rules:
     - a tween whose element is not shown (not under the scene of the last SHOW) does not run: its time stands
       still from the SHOW that hid it (or its making) to the SHOW (or ADD) that shows it again. The pause and the
       resume take the ticks' time: the pulse whose wake-up applies the batch (else the time it is applied), never
       before the last tick, so a scene shown again goes on from the very value it was left at;
     - a SET of a property a tween runs stops that tween on it (the tween goes on with its other properties and
       still ends, with its done); with ease on the property (EASE) and the element shown, the SET starts a tween of
       the ease's ms and curve from the value now instead (none when an earlier ease tween already goes to that
       value; a value that is not a number or a solid colour is set at once). A SET of x then y (a move) is applied
       as one move (one dirty pair) unless ease covers x or y;
     - a new tween takes its properties over from the tweens that run on the same element (a chained one when it
       starts);
     - a tween left without properties by such writes or takeovers runs on to its end and its done; an endless one
       (repeat -1) is stopped at once instead, without a done (nothing would need its frames, it never ends);
     - REMOVE of an element stops its tweens, without a done, unless an ADD puts it back in the same delivery (a
       move). FREE: the same, and its ease settings go. In both cases the tweens chained after a stopped one stop
       too, without a done, whatever their elements (they would wait forever; `after` naming a tween that is gone
       starts at once instead). The tweens of the element's children (a REMOVE of a group, or the children a FREE
       of a group detaches) are not stopped: like every tween they follow their element's shown state, paused
       while it is not shown and resumed when it is shown again (and stopped when it is freed);
     - TWEEN_STOP stops a tween and those chained after it, without a done; with to_end they write their final
       values first (slate_tween_final);
     - a TWEEN naming a property twice is refused (counted in cmd_errors);
     - a tween that ends by itself (after all its repeats; never for repeat -1) puts its id into the done queue,
       in the order the tweens ended; they are taken with slate_engine_tween_done. The queue is in external RAM,
       grown as needed up to 4096 ids waiting (more are lost, counted in tween_dones_lost). When it goes from
       empty to not empty the engine calls the tween listener from its task (outside any lock): it may drain the
       queue and raise events (the PHP layer does: one event of ext-events per id, so a burst of N ends costs N
       small allocations and emits on this task), never call PHP, never block. The PHP task drains it too (wait()):
       events may then come in an order other than the end order.
   While the panel sleeps or after a failure no frame goes, but a running tween still ends on time: the task wakes
   at the earliest end and writes its final values (its done is told).

   Transitions (compose.h, Transitions). A SHOW with a transition (cmd.h, slate_trans_t: FADE, PUSH, COVER or
   UNCOVER, a direction, ms, an ease) of a root other than the one shown starts one, from the scene shown (or none:
   its background only) to the new one, at the batch's time (as the tweens': the pulse that comes with the batch,
   else the time it is applied). Both scenes are composed together, each in a layer under the screen root, band by
   band in the same two bands (no screen-sized buffer more; a fade composes the new scene offscreen in strips of the
   scratch, a band's worth at most, like a group with opacity: when internal RAM refuses even one row of it, the new
   scene is drawn at full opacity, a cut instead of a fade, counted in stats.last.degraded). Each scene is cut to its
   page, and its caches are built for its page. At every tick (the tweens' ticks: the pulse that
   starts the frame, a pulse asked for while it runs) the layers go where the eased time puts them, offsets rounded
   to whole pixels, and the whole screen is dirty when they moved (a tick that leaves them on the same pixel, or a
   fade at the same opacity, gives no frame unless something else changed). The tweens of both scenes run (both are shown, also for eased
   SETs); group caches are used and move with their scene. The underlay is given back at the start and not used
   until the end. The target's background is the bottom scene's. At the tick at or past its end (also while the
   panel sleeps: the task wakes for it) it ends: the new scene alone under the screen, its background, a full frame
   of it; the scene left is detached, its caches kept or dropped (see Kept scenes) and its tweens paused (as for a
   SHOW at once); its done (the two roots' ids) goes to the transition done queue (TR_DONE_MAX = 64 waiting; more
   are lost, counted) and the tween listener is called when that queue was empty (it drains both queues:
   slate_engine_transition_done). Then:
     - a SHOW during a transition (with a transition or not, of any root but the one it goes to) first ends it
       (jumps to its end, with its done), then is applied as usual: from the new scene, whole;
     - a SHOW of the root it goes to goes on with it: only the name and the background change (that scene's page
       in the new colour), as for a SHOW of the root shown without a transition (as before); a SHOW that is refused
       (a group inside another one) does not end it;
     - REMOVE of either root is refused (as for the scene shown); FREE of either ends it first (its done);
     - a transition that cannot begin (no memory for its layers) is a cut, as a SHOW without one, and still queues
       its done at once (with the listener), so the PHP side ends it;
     - with nothing running, a scene without a transition costs no work per frame (a flag).

   Kept scenes. A scene left (a SHOW at once, or the end of a transition) whose SHOW said keep (cmd.h: the PHP
   layer's Scene::$keepCache of the scene left) keeps its element caches (cached groups, shadows, blurs: the images
   it would build again when shown again) instead of dropping them, while there is room: the root goes to the end of
   a list of kept roots, oldest first. They stay where they are: in the cache budget, in external RAM; nothing more
   is allocated for them (the list itself: a few pointers, external RAM). The limits (slate_engine_set_keep): at most
   `scenes` roots kept (0: none, every scene left drops its caches, as before; the default) and, when budget is not
   0, at most budget bytes of their caches; past either, the oldest kept root drops its caches at once (a scene
   left whose caches alone are more than the budget drops them at once). A root without caches is not kept. When a
   cache allocation (an element of the shown scene, the underlay, a blur's temporary memory: every cache_alloc)
   does not fit in the budget, or external RAM refuses it, the kept roots drop their caches, oldest first, until it
   fits; nothing is dropped for an allocation that would not fit with every kept cache gone. So kept caches never
   take room from the scene shown. A SHOW of a kept root takes it off the list (its caches are used at once, also
   by a transition to it); so does an ADD of it into a group (its caches go with it); FREE of a kept root, or of an
   element inside one, and REMOVE of an element from one, drop the caches under that element first (nothing cached
   leaves the list's reach); an ADD into a kept scene counts the caches brought in (the limits apply).
   Image memory is not part of the cache budget (Images): an image does not take room in it. But when the image
   loader runs out of memory for an image (images.h, Memory), it asks the engine for the kept caches: the loader
   task sets a flag and kicks the task (it never touches the caches itself), the task drops every kept cache at its
   next wake-up (oldest first) and answers on a queue (with whether it freed anything); the loader waits at most
   SLATE_RECLAIM_WAIT_MS (100 ms) for the answer, then reads the file once more when something was freed or no
   answer came (only then NO_MEMORY). Without an engine (none started, or stopping: slate_engine_stop first cuts the
   loader off and waits for a request under way, by the clock, in waits of SLATE_NAP_MS that really block) or with
   nothing kept, the loader fails at once, as before. No PHP, no idle hook,
   no work per frame. Lowering the limits applies at the task's next wake-up (kicked), and a scene left
   always sees the limits set before its SHOW was pushed.
   Stats: kept_scenes, kept_bytes, kept_dropped (roots whose caches were dropped while kept), reclaims (the
   loader's requests served). With nothing kept, no work per frame.

   Scrolls (scroll.h, scene.h "Scroll"). SCROLL commands (cmd.h, slate_scroll_cmd_t) drive a GROUP that scrolls; the
   engine keeps a small record for each such scroll (external RAM, made by its first SCROLL command, gone with FREE).
     - CONFIG makes the group a scroll (slate_scroll_init with its axis, bounce, snaps, snap_max; position 0, 0), or
       changes the set-up of one (a change of axis, bounce, snap or snap_max stops what moves, without an end, and
       recomputes the limits; the same set-up again changes nothing: a scrollTo goes on from where it is); it also
       says whether the bars show (indicators) and where the position is published (pub: the engine takes a
       reference, given back at FREE, when the group stops being a scroll, or at stop).
     - PRESS, DRAG, RELEASE carry the time PHP saw the touch (t_us): the physics takes its velocity from those
       times. PRESS stops any motion where it is (slate_scroll_press) and a scrollTo like TWEEN_STOP (no done, no
       end); RELEASE starts an inertia, a snap or a spring; a release after a drag that leaves nothing to move ends at
       once (an end). JUMP sets the position (clamped to the limits), stops every motion and a scrollTo, no end.
     - scrollTo is a TWEEN of the scroll's SCROLL_X / SCROLL_Y (only the engine animates them: slate_el_anim_get
       reads a scroll's drawn position). Its targets are clamped to the limits when it starts (applied, or at the end
       of the tween it is chained after) and again whenever the limits shrink under it, so it ends inside them (a
       locked axis has a limit of 0: neither a scrollTo nor a JUMP moves it); it stops a motion (no end) and takes
       over from an earlier scrollTo (which runs on to its done, without an end). Ending by itself it gives its done
       (Tweens) and then one scroll end (queued after the tween dones of that tick); stopped (TWEEN_STOP, PRESS, JUMP,
       REMOVE, FREE, a new set-up) neither.
     - At every tick (the tweens' ticks: the frame's time, never the time spent computing), after the tweens and
       before the frame, each shown scroll that moves by itself is stepped (slate_scroll_step, its limits brought up
       to date first: rows added or removed, a view or content size changed, move its edges) and its position is
       written (SLATE_PROP_SCROLL_X / Y with slate_el_set: the view dirty); a motion that ends queues one end.
     - A scroll's time stands still while it is not shown (as a tween's: its clock is the ticks' time minus the time
       spent hidden): at the SHOW that hides it, it is stepped to that instant; shown again, it goes on from there.
       During a transition both scenes are shown: it goes on. A drag whose Released cannot come (the scroll no longer
       shown: a SHOW of another scene, a REMOVE or FREE of an ancestor; or a transition started: the PHP side ignores
       the touch meanwhile) ends as a release without velocity (slate_scroll_cancel: a spring back from past an edge,
       a snap, or rest; the drag's end), then pauses with its scene. REMOVE of the scroll itself (not put back in the
       same delivery) stops its motion and drag, without an end, and its bars go; FREE drops it, without an end. After every delivery a scroll at
       rest is brought into its limits when they moved (rows removed under it).
     - Bars (CONFIG indicators): INDICATOR_A 1 while the content moves (drag, inertia, snap, spring, scrollTo), held
       SC_HOLD_US (500 ms, the scroll's clock) after it stops, then linear to 0 in SC_FADE_US (200 ms). During the
       hold nothing ticks: the task sleeps until its end (a timed wake-up), then ticks while the bars fade; when no
       scroll moves or fades it does nothing per frame for them, and a still screen gives no frame.
     - Published position: after every change the engine writes the rounded drawn position and the moving flag into
       the scroll's pub (cmd.h, slate_scroll_pub_t: two int16 in one atomic word; moving while a drag, an inertia, a
       snap, a spring or a scrollTo runs), and after a JUMP or CONFIG its seq: the PHP task reads them without a lock.
       The pub belongs to whoever holds a reference (the PHP record, the commands that name it, the engine): it
       outlives the element, so a read after FREE is safe (the last values).
     - Scroll ends: (element id, x, y: the position drawn then) into the scroll end queue, in the order they came,
       taken with slate_engine_scroll_done; the same policy as the tween done queue (external RAM, grown up to 4096
       waiting, more lost and counted in tween_dones_lost); when it goes from empty to not empty the engine calls
       the tween listener.
     - While the panel sleeps (no frames) a moving scroll is stepped every SC_IDLE_TICK_US (50 ms) until it stops.
   Stats: scrolls_active (and anim_ticks, the ticks of every animation, counted without the engine's lock). With
   slate-debug, the first frame that composes a cached scroll directly (its content image refused: stats
   scroll_nocache) prints one line, once per engine.

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
#define SLATE_ENGINE_STACK 10240       /* bytes */
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
    uint32_t tweens_active;      /* tweens alive: running, waiting for the tween they are chained after, or paused
                                    (their element not shown) */
    uint32_t tween_dones_lost;   /* done ids dropped: the queue was full (or out of memory) */
    uint32_t transitions;        /* transitions ended (each with its done) */
    uint32_t transition_dones_lost;   /* transition dones dropped: TR_DONE_MAX (64) were waiting */
    uint32_t kept_scenes;        /* scenes left that keep their caches now (see Kept scenes) */
    size_t kept_bytes;           /*   their cache memory (part of cache_bytes) */
    uint32_t kept_dropped;       /* kept scenes whose caches were dropped: past the limits, or for room */
    uint32_t reclaims;           /* the image loader's requests for the kept caches served (see Kept scenes) */
    uint32_t scrolls_active;     /* scrolls in motion (drag, inertia, snap, spring, scrollTo) or with bars fading,
                                    paused ones included (see Scrolls) */
    uint32_t anim_ticks;         /* ticks of the animations (tweens, transitions, scrolls) since the start */
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
   is stuck in the driver: everything stays, and no engine can start again), or when an image loader's reclaim
   (Kept scenes) did not end within a second more than SLATE_RECLAIM_WAIT_MS (e and its queues stay allocated;
   another engine may start). After false, slate_engine_error(e) gives the reason (e stays allocated): the panel's
   ("the panel did not finish a band within 2000 ms") or "an image load waiting for memory did not end in time".
   NULL allowed (true). PHP task only. */
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
/* The limits of the kept scenes (see Kept scenes): at most scenes roots (< 0 taken as 0; 0: none, the default) and,
   when budget_bytes is not 0, at most budget_bytes of their caches. Applied at the task's next wake-up (this call
   wakes it): lowering them drops the oldest kept caches then. Any task. */
void slate_engine_set_keep(slate_engine_t *e, int scenes, size_t budget_bytes);

/* Takes the id of the oldest tween that ended by itself (see Tweens) into *tween_id; false when none waits. Any
   task (not an ISR), under the engine's lock: the PHP task, and the tween listener (it may drain the queue). */
bool slate_engine_tween_done(slate_engine_t *e, uint32_t *tween_id);
/* The function the engine task calls when the done queue goes from empty to not empty (arg passed back), NULL for
   none. Read by slate_engine_start: set it before (the PHP layer at its MINIT). cb runs on the engine task: it may
   drain the queue (slate_engine_tween_done) and wake the PHP side, no PHP, no blocking. PHP task. */
void slate_engine_set_tween_listener(void (*cb)(void *arg), void *arg);

/* Takes the oldest transition that ended (see Transitions): the ids of the roots it went from (0: none was shown)
   and to; false when none waits. Any task (not an ISR), under the engine's lock: the PHP task, and the listener. */
bool slate_engine_transition_done(slate_engine_t *e, uint32_t *from_root, uint32_t *to_root);
/* True while a transition runs in the engine (from the wake-up that applies its SHOW to the tick that ends it): the
   PHP task ignores the touch meanwhile. A SHOW pushed and not applied yet does not count. Any task. */
bool slate_engine_in_transition(slate_engine_t *e);

/* Takes the oldest scroll end (see Scrolls): the scroll's element id and its position then (whole pixels); false
   when none waits. Any task (not an ISR), under the engine's lock: the PHP task, and the tween listener. */
bool slate_engine_scroll_done(slate_engine_t *e, uint32_t *el_id, int *x, int *y);

#endif
