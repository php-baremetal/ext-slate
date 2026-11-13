/*
   +----------------------------------------------------------------------+
   | php-baremetal: ext-slate                                             |
   +----------------------------------------------------------------------+
   | Copyright (c) 2026 Gianfrancesco Aurecchia                           |
   +----------------------------------------------------------------------+
   | This source file is subject to the MIT license, that is bundled with |
   | this package in the file LICENSE.                                    |
   +----------------------------------------------------------------------+
   | Authors: Gianfrancesco Aurecchia <gianfri@php-baremetal.com>         |
   +----------------------------------------------------------------------+
*/

/*
 * Images in PHP: the element Image (its constructor and $src are in php_slate_elements.c; here its image, wait()
 * and its readonly w, h, loaded), Images::preload() and Preload, the events ImageLoaded, ImageFailed,
 * PreloadProgress, PreloadDone and the enum ImageError.
 *
 * Images are loaded by the store's loader task (images.h). When one is done the store calls our listener on that
 * task: it kicks the running engine (the IMAGE elements that show it are synced and drawn without any delivery),
 * wakes a wait(), and queues the image (one reference) in a list of finished images and an ImageLoaded or
 * ImageFailed event of ext-events whose 16 bytes of data carry the image's pointer. The listener never calls
 * PHP.
 *
 * On the PHP task the event's filler takes the image out of the list (so the pointer is alive: the list's
 * reference), updates every Image and Preload that holds it (w, h, loaded; a Preload's loaded and done, and its
 * own events PreloadProgress and PreloadDone, queued behind), fills the event and drops the reference. So the
 * properties change when the event is delivered, as the events of other sources, or in wait(), which reads the
 * state of its own image(s) without delivering anything.
 *
 * Events that are never delivered (the init-loop runtime gives the slots back unread; a full event pool drops
 * them) would leave their image in the list for good: a hook run when the PHP task returns to the event loop
 * (bm_events_on_idle) finds the images whose events are no longer queued (no listener is between its append and
 * its emit, and ext-events counts no queued event of the two classes) and applies them the same way, with the
 * Preload events (an ImageLoaded lost in a burst does not lose the PreloadDone), and emits again a PreloadDone
 * the pool refused: every Preload gets exactly one.
 *
 * Image memory and the store are C (images.c); here only the PHP objects, which hold references: an Image its
 * current image, a Preload all of its images, until they are freed.
 */
#include <string.h>

#include "php.h"
#include "zend_enum.h"
#include "zend_exceptions.h"
#include "ext/spl/spl_exceptions.h"

#include "bm_events_hal.h"
#include "event_bus_php.h"
#include "halpool.h"
#include "images.h"
#include "php_slate.h"
#include "slate_config.h"

zend_class_entry *sl_images_ce, *sl_preload_ce, *sl_image_error_ce, *sl_image_loaded_ce, *sl_image_failed_ce,
    *sl_preload_progress_ce, *sl_preload_done_ce;

static zend_object_handlers preload_handlers;

/* The readonly properties of Image (slot offsets). */
static uint32_t s_off_w, s_off_h, s_off_loaded;

/* ---- the shared state (loader task and PHP task) ---- */

enum { EV_LOADED, EV_FAILED, EV_PROGRESS, EV_DONE, EV_N };

typedef struct sl_done {
    slate_image_t *im;      /* one reference */
    struct sl_done *next;
} sl_done_t;

static bm_hal_lock_t *s_lock;     /* the list, s_emitting, s_accepting; created once, never freed */
static bm_hal_queue_t *s_wake;    /* a token per finished image, for wait(); created once */
static sl_done_t *s_head, *s_tail;
static int s_emitting;            /* listeners between their append and the end of their emit */
static bool s_accepting;          /* a request runs with the events set up */
static uint16_t s_tag[EV_N];

/* The engine the listener kicks, and the listeners inside a kick (sl_images_engine waits for them). */
static slate_engine_t *s_kick_engine;
static int s_kick_users;

/* ---- the PHP task's state (request memory) ---- */

static bool s_ready;              /* the events and registries are set up for this request */
static HashTable s_images;        /* handle -> sl_obj_t* of the Images that hold an image */
static HashTable s_preloads;      /* handle -> sl_preload_t* */
static HashTable s_done_failed;   /* serial of a PreloadDone in flight -> its $failed */
static uint32_t s_done_serial;

typedef struct {
    uint32_t n;
    slate_image_t **ims;     /* n references (emalloc) */
    uint8_t *flags;          /* per image: PL_SEEN (counted in the properties), PL_ANNOUNCED (in the events) */
    uint32_t seen, seen_ok;  /* finished / loaded, as the properties show */
    uint32_t ann, ann_ok;    /* finished / loaded, as the events told */
    bool done_sent;
    zend_object std;
} sl_preload_t;

#define PL_SEEN 1
#define PL_ANNOUNCED 2

static sl_preload_t *preload_of(zend_object *o)
{
    return (sl_preload_t *)((char *)o - XtOffsetOf(sl_preload_t, std));
}

static bm_hal_lock_t *lock(void)
{
    bm_hal_lock_t *l = __atomic_load_n(&s_lock, __ATOMIC_ACQUIRE);
    bm_hal_lock_enter(l);
    return l;
}

/* ---- the listener (loader task) ---- */

/* Queues im (a reference) and its event. Any task. */
static void queue_done(slate_image_t *im)
{
    sl_done_t *n = bm_hal_alloc(sizeof *n, BM_HAL_MEM_EXTERNAL);
    if (!n) {
        return;   /* no event: wait() still sees the state */
    }
    slate_image_ref(im);
    n->im = im;
    n->next = NULL;
    bm_hal_lock_t *l = lock();
    if (!s_accepting) {
        bm_hal_lock_exit(l);
        slate_image_put(im);
        bm_hal_free(n);
        return;
    }
    if (s_tail) {
        s_tail->next = n;
    } else {
        s_head = n;
    }
    s_tail = n;
    s_emitting++;
    uint16_t tag = slate_image_state(im) == SLATE_IMAGE_READY ? s_tag[EV_LOADED] : s_tag[EV_FAILED];
    bm_hal_lock_exit(l);
    bm_events_emit_data(tag, NULL, &im, sizeof im);   /* dropped: the idle hook applies it */
    l = lock();
    s_emitting--;
    bm_hal_lock_exit(l);
}

static void on_done(slate_image_t *im, void *arg)
{
    (void)arg;
    __atomic_add_fetch(&s_kick_users, 1, __ATOMIC_SEQ_CST);
    slate_engine_t *e = __atomic_load_n(&s_kick_engine, __ATOMIC_SEQ_CST);
    if (e) {
        slate_engine_kick(e);
    }
    __atomic_sub_fetch(&s_kick_users, 1, __ATOMIC_SEQ_CST);
    queue_done(im);
    uint8_t t = 0;
    bm_hal_queue_send(s_wake, &t, 0);
}

void sl_images_engine(slate_engine_t *e)
{
    __atomic_store_n(&s_kick_engine, e, __ATOMIC_SEQ_CST);
    while (!e && __atomic_load_n(&s_kick_users, __ATOMIC_SEQ_CST) > 0) {
        uint8_t t;
        /* lets a listener of a lower priority finish its kick: a real block (1 ms would be 0 ticks at 100 Hz, a
           busy loop that starves it on the same core) */
        bm_hal_queue_receive(s_wake, &t, SLATE_NAP_MS);
    }
}

/* ---- applying a finished image to the PHP objects ---- */

static void image_state(sl_obj_t *o)
{
    zval *w = OBJ_PROP(&o->std, s_off_w), *h = OBJ_PROP(&o->std, s_off_h), *ld = OBJ_PROP(&o->std, s_off_loaded);
    const slate_sli_t *sli = o->im && slate_image_state(o->im) == SLATE_IMAGE_READY ? slate_image_sli(o->im) : NULL;
    ZVAL_LONG(w, sli ? sli->w : 0);
    ZVAL_LONG(h, sli ? sli->h : 0);
    ZVAL_BOOL(ld, sli != NULL);
}

static zval *pl_prop(sl_preload_t *pl, const char *name)
{
    zend_property_info *info = zend_hash_str_find_ptr(&sl_preload_ce->properties_info, name, strlen(name));
    return OBJ_PROP(&pl->std, info->offset);
}

static void preload_props(sl_preload_t *pl)
{
    ZVAL_LONG(pl_prop(pl, "loaded"), pl->seen_ok);
    ZVAL_LONG(pl_prop(pl, "total"), pl->n);
    ZVAL_BOOL(pl_prop(pl, "done"), pl->seen == pl->n);
}

/* PreloadDone of pl, once: false when the event pool refused it (the idle hook tries again). */
static bool emit_done(sl_preload_t *pl)
{
    zval failed;
    array_init(&failed);
    for (uint32_t i = 0; i < pl->n; i++) {
        if (slate_image_state(pl->ims[i]) == SLATE_IMAGE_FAILED) {
            add_next_index_string(&failed, slate_image_path(pl->ims[i]));
        }
    }
    uint32_t serial = ++s_done_serial;
    zend_hash_index_update(&s_done_failed, serial, &failed);
    if (!bm_events_emit_data(s_tag[EV_DONE], NULL, &serial, sizeof serial)) {
        zend_hash_index_del(&s_done_failed, serial);
        return false;
    }
    pl->done_sent = true;
    return true;
}

/* Image i of pl is done: counted in the properties; with announce, in the events as well (PreloadProgress for a
   loaded one, PreloadDone after the last). */
static void preload_mark(sl_preload_t *pl, uint32_t i, bool announce)
{
    slate_image_state_t st = slate_image_state(pl->ims[i]);
    if (st == SLATE_IMAGE_LOADING) {
        return;
    }
    if (!(pl->flags[i] & PL_SEEN)) {
        pl->flags[i] |= PL_SEEN;
        pl->seen++;
        pl->seen_ok += st == SLATE_IMAGE_READY;
        preload_props(pl);
    }
    if (announce && !(pl->flags[i] & PL_ANNOUNCED)) {
        pl->flags[i] |= PL_ANNOUNCED;
        pl->ann++;
        if (st == SLATE_IMAGE_READY) {
            uint32_t d[2] = { ++pl->ann_ok, pl->n };
            bm_events_emit_data(s_tag[EV_PROGRESS], NULL, d, sizeof d);
        }
        if (pl->ann == pl->n && !pl->done_sent) {
            emit_done(pl);
        }
    }
}

/* im is done: every Image and Preload that holds it. Plain C, also from the idle hook (the events it emits are
   C calls of ext-events). */
static void apply(slate_image_t *im, bool announce)
{
    void *ptr;
    ZEND_HASH_FOREACH_PTR(&s_images, ptr) {
        sl_obj_t *o = ptr;
        if (o->im == im) {
            image_state(o);
        }
    } ZEND_HASH_FOREACH_END();
    ZEND_HASH_FOREACH_PTR(&s_preloads, ptr) {
        sl_preload_t *pl = ptr;
        for (uint32_t i = 0; i < pl->n; i++) {
            if (pl->ims[i] == im) {
                preload_mark(pl, i, announce);
            }
        }
    } ZEND_HASH_FOREACH_END();
}

/* Takes im out of the finished list (NULL: not there). PHP task. */
static sl_done_t *take(slate_image_t *im)
{
    bm_hal_lock_t *l = lock();
    sl_done_t *prev = NULL, *n = s_head;
    while (n && n->im != im) {
        prev = n;
        n = n->next;
    }
    if (n) {
        if (prev) {
            prev->next = n->next;
        } else {
            s_head = n->next;
        }
        if (s_tail == n) {
            s_tail = prev;
        }
    }
    bm_hal_lock_exit(l);
    return n;
}

static void drop(sl_done_t *n)
{
    slate_image_put(n->im);
    bm_hal_free(n);
}

/* Back in the event loop (plain C, from Slate's idle hook): the finished images whose events are gone undelivered. */
void sl_images_idle(void)
{
    if (!s_ready) {
        return;
    }
    bm_hal_lock_t *l = lock();
    sl_done_t *last = s_emitting == 0 ? s_tail : NULL;
    bm_hal_lock_exit(l);
    evt_tag_stats_t st;
    if (last) {
        evt_tag_stats(s_tag[EV_LOADED], &st);
        uint32_t q = st.queued;
        evt_tag_stats(s_tag[EV_FAILED], &st);
        if (q == 0 && st.queued == 0) {
            /* every image up to last had its event emitted, and none is queued: they were given back or dropped */
            l = lock();
            sl_done_t *list = s_head;
            s_head = last->next;
            if (!s_head) {
                s_tail = NULL;
            }
            last->next = NULL;
            bm_hal_lock_exit(l);
            while (list) {
                sl_done_t *next = list->next;
                apply(list->im, true);   /* the Preload events still come: they do not depend on the image's */
                drop(list);
                list = next;
            }
        }
    }
    /* a PreloadDone the pool refused: again, until it goes */
    void *ptr;
    ZEND_HASH_FOREACH_PTR(&s_preloads, ptr) {
        sl_preload_t *pl = ptr;
        if (pl->ann == pl->n && !pl->done_sent) {
            emit_done(pl);
        }
    } ZEND_HASH_FOREACH_END();
    if (zend_hash_num_elements(&s_done_failed) > 0) {
        evt_tag_stats(s_tag[EV_DONE], &st);
        if (st.queued == 0) {
            zend_hash_clean(&s_done_failed);
        }
    }
}

/* ---- the events' fillers (PHP task, before the delivery) ---- */

static void set_str(zend_object *ev, zend_class_entry *ce, const char *name, const char *s)
{
    zval v;
    ZVAL_STRING(&v, s);
    bm_events_set_zval(ev, ce, name, &v);
    zval_ptr_dtor(&v);
}

static const char *const err_case[] = {
    [SLATE_IMG_NOT_FOUND] = "NotFound", [SLATE_IMG_UNREADABLE] = "Unreadable", [SLATE_IMG_BAD_FORMAT] = "BadFormat",
    [SLATE_IMG_UNSUPPORTED] = "Unsupported", [SLATE_IMG_NO_MEMORY] = "NoMemory",
};

static const char *err_message(slate_img_err_t e)
{
    switch (e) {
    case SLATE_IMG_NOT_FOUND:
        return "no such file";
    case SLATE_IMG_UNREADABLE:
        return "the file could not be read";
    case SLATE_IMG_BAD_FORMAT:
        return "not a valid image of its format (.sli or PNG): a damaged or truncated file";
    case SLATE_IMG_UNSUPPORTED:
        return SLATE_HAS_PNG ? "a PNG that is not decoded on the board (16 bits per channel, interlaced, or larger "
                               "than 32767 pixels): convert it to .sli with phpbm"
                             : "png is not built in: add it to slate-images in composer.json";
    case SLATE_IMG_NO_MEMORY:
        return "not enough memory for the image";
    default:
        return "the image could not be loaded";
    }
}

static slate_image_t *slot_image(const evt_t *slot)
{
    size_t len;
    const void *d = bm_events_slot_data(slot, &len);
    slate_image_t *im = NULL;
    if (d && len == sizeof im) {
        memcpy(&im, d, sizeof im);
    }
    return im;
}

static void fill_image(zend_object *ev, const evt_t *slot, bool failed)
{
    slate_image_t *im = slot_image(slot);
    sl_done_t *n = im && s_ready ? take(im) : NULL;
    zend_class_entry *ce = failed ? sl_image_failed_ce : sl_image_loaded_ce;
    set_str(ev, ce, "path", n ? slate_image_path(im) : "");
    if (!failed) {
        const slate_sli_t *sli = n ? slate_image_sli(im) : NULL;
        bm_events_set_long(ev, ce, "w", sli ? sli->w : 0);
        bm_events_set_long(ev, ce, "h", sli ? sli->h : 0);
    } else {
        slate_img_err_t e = n ? slate_image_error(im) : SLATE_IMG_UNREADABLE;
        if (e < SLATE_IMG_NOT_FOUND || e > SLATE_IMG_NO_MEMORY) {
            e = SLATE_IMG_UNREADABLE;
        }
        zval c;
        ZVAL_OBJ(&c, zend_enum_get_case_cstr(sl_image_error_ce, err_case[e]));
        bm_events_set_zval(ev, ce, "error", &c);
        set_str(ev, ce, "message", err_message(e));
    }
    if (n) {
        apply(im, true);
        drop(n);
    }
}

static void fill_loaded(zend_object *ev, zend_object *dev, const evt_t *slot)
{
    (void)dev;
    fill_image(ev, slot, false);
}

static void fill_failed(zend_object *ev, zend_object *dev, const evt_t *slot)
{
    (void)dev;
    fill_image(ev, slot, true);
}

static void fill_progress(zend_object *ev, zend_object *dev, const evt_t *slot)
{
    (void)dev;
    size_t len;
    const void *d = bm_events_slot_data(slot, &len);
    uint32_t v[2] = { 0, 0 };
    if (d && len == sizeof v) {
        memcpy(v, d, sizeof v);
    }
    bm_events_set_long(ev, sl_preload_progress_ce, "loaded", (long)v[0]);
    bm_events_set_long(ev, sl_preload_progress_ce, "total", (long)v[1]);
}

static void fill_done(zend_object *ev, zend_object *dev, const evt_t *slot)
{
    (void)dev;
    size_t len;
    const void *d = bm_events_slot_data(slot, &len);
    uint32_t serial = 0;
    if (d && len == sizeof serial) {
        memcpy(&serial, d, sizeof serial);
    }
    zval *failed = s_ready ? zend_hash_index_find(&s_done_failed, serial) : NULL;
    if (failed) {
        bm_events_set_zval(ev, sl_preload_done_ce, "failed", failed);
        zend_hash_index_del(&s_done_failed, serial);
    } else {
        zval empty;
        array_init(&empty);
        bm_events_set_zval(ev, sl_preload_done_ce, "failed", &empty);
        zval_ptr_dtor(&empty);
    }
}

static const bm_event_filler_t s_fillers[EV_N] = {
    [EV_LOADED] = fill_loaded, [EV_FAILED] = fill_failed, [EV_PROGRESS] = fill_progress, [EV_DONE] = fill_done,
};

/* ---- setup and teardown ---- */

/* The first image of the request: the lock and the wake queue (once), the event classes as sources of ext-events,
   the registries, the idle hook. false with an exception. */
static bool ready(void)
{
    if (s_ready) {
        return true;
    }
    if (!s_lock) {
        bm_hal_lock_t *l = bm_hal_lock_create();
        bm_hal_queue_t *q = l ? bm_hal_queue_create(1, 1) : NULL;
        if (!q) {
            zend_throw_exception(spl_ce_RuntimeException, "Slate: no memory for the image loader", 0);
            return false;   /* the lock (if any) is kept: never freed */
        }
        s_wake = q;
        __atomic_store_n(&s_lock, l, __ATOMIC_RELEASE);
    }
    zend_class_entry *ces[EV_N] = { sl_image_loaded_ce, sl_image_failed_ce, sl_preload_progress_ce,
                                    sl_preload_done_ce };
    uint16_t tags[EV_N];
    for (int i = 0; i < EV_N; i++) {
        if (!bm_events_source_tag(ces[i]->name, &tags[i])) {
            if (!EG(exception)) {
                zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                                        "Slate: cannot raise %s: too many event sources", ZSTR_VAL(ces[i]->name));
            }
            return false;
        }
    }
    for (int i = 0; i < EV_N; i++) {
        s_tag[i] = tags[i];
        bm_events_set_filler(tags[i], s_fillers[i]);
    }
    if (!sl_idle_use(SL_IDLE_IMAGES, true)) {
        /* ext-events keeps 4 hooks; Slate takes 1 for the running Slate, the images and the animations */
        for (int i = 0; i < EV_N; i++) {
            bm_events_set_filler(tags[i], NULL);
        }
        zend_throw_exception(spl_ce_RuntimeException,
                             "Slate: cannot load images: the table of ext-events' idle hooks is full", 0);
        return false;
    }
    zend_hash_init(&s_images, 8, NULL, NULL, 0);
    zend_hash_init(&s_preloads, 4, NULL, NULL, 0);
    zend_hash_init(&s_done_failed, 4, NULL, ZVAL_PTR_DTOR, 0);
    bm_hal_lock_t *l = lock();
    s_accepting = true;
    bm_hal_lock_exit(l);
    s_ready = true;
    return true;
}

void sl_images_rshutdown(void)
{
    if (!s_ready) {
        return;
    }
    s_ready = false;
    sl_idle_use(SL_IDLE_IMAGES, false);
    bm_hal_lock_t *l = lock();
    s_accepting = false;
    sl_done_t *list = s_head;
    s_head = s_tail = NULL;
    bm_hal_lock_exit(l);
    while (list) {
        sl_done_t *next = list->next;
        drop(list);
        list = next;
    }
    for (int i = 0; i < EV_N; i++) {
        bm_events_set_filler(s_tag[i], NULL);
    }
    /* objects freed later keep their references and drop them themselves; they are no longer listed */
    zend_hash_destroy(&s_images);
    zend_hash_destroy(&s_preloads);
    zend_hash_destroy(&s_done_failed);
}

/* ---- paths ---- */

typedef enum { PATH_OK, PATH_NOT_ABSOLUTE, PATH_BAD_EXT, PATH_NO_PNG } path_check_t;

static bool ends_ci(const char *s, size_t len, const char *ext)
{
    size_t n = strlen(ext);
    if (len < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[len - n + i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != ext[i]) {
            return false;
        }
    }
    return true;
}

static path_check_t path_check(const zend_string *p)
{
    const char *s = ZSTR_VAL(p);
    size_t len = ZSTR_LEN(p);
    if (len == 0 || s[0] != '/' || strlen(s) != len) {
        return PATH_NOT_ABSOLUTE;
    }
    if (ends_ci(s, len, ".png")) {
        return SLATE_HAS_PNG ? PATH_OK : PATH_NO_PNG;
    }
    return ends_ci(s, len, ".sli") ? PATH_OK : PATH_BAD_EXT;
}

static void png_not_built(void)
{
    zend_value_error("png is not built in: add it to slate-images in composer.json");
}

bool sl_image_src_ok(const zval *v)
{
    if (Z_TYPE_P(v) != IS_STRING) {
        return false;
    }
    switch (path_check(Z_STR_P(v))) {
    case PATH_OK:
        return true;
    case PATH_NOT_ABSOLUTE:
        zend_value_error("Baremetal\\Slate\\Image::$src must be an absolute path");
        return false;
    case PATH_BAD_EXT:
        zend_value_error("Baremetal\\Slate\\Image::$src must end in .sli or .png");
        return false;
    case PATH_NO_PNG:
        png_not_built();
        return false;
    }
    return false;
}

/* ---- Image ---- */

void sl_image_class(zend_class_entry *ce)
{
    static const char *const names[] = { "w", "h", "loaded" };
    uint32_t *offs[] = { &s_off_w, &s_off_h, &s_off_loaded };
    for (int i = 0; i < 3; i++) {
        zval undef;
        ZVAL_UNDEF(&undef);
        zend_type t = i < 2 ? (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 0, 0)
                            : (zend_type)ZEND_TYPE_INIT_CODE(_IS_BOOL, 0, 0);
        zend_property_info *info = zend_declare_typed_property(
            ce, zend_string_init_interned(names[i], strlen(names[i]), 1), &undef, ZEND_ACC_PUBLIC | ZEND_ACC_READONLY,
            NULL, t);
        *offs[i] = info->offset;
    }
}

void sl_image_init(sl_obj_t *o)
{
    ZVAL_LONG(OBJ_PROP(&o->std, s_off_w), 0);
    ZVAL_LONG(OBJ_PROP(&o->std, s_off_h), 0);
    ZVAL_FALSE(OBJ_PROP(&o->std, s_off_loaded));
}

bool sl_image_failed(const sl_obj_t *o)
{
    return o->im && slate_image_state(o->im) == SLATE_IMAGE_FAILED;
}

static slate_image_t *get(zend_string *path)
{
    slate_image_t *im = slate_image_get(ZSTR_VAL(path));
    if (!im) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, "Slate: no memory for the image %s", ZSTR_VAL(path));
    }
    return im;
}

bool sl_image_bind(sl_obj_t *o, zend_string *path)
{
    if (!ready()) {
        return false;
    }
    slate_image_t *im = get(path);
    if (!im) {
        return false;
    }
    slate_image_t *old = o->im;
    o->im = im;
    zend_hash_index_update_ptr(&s_images, o->std.handle, o);
    image_state(o);
    slate_image_put(old);
    return true;
}

void sl_image_unbind(sl_obj_t *o)
{
    if (!o->im) {
        return;
    }
    if (s_ready) {
        zend_hash_index_del(&s_images, o->std.handle);
    }
    slate_image_put(o->im);
    o->im = NULL;
}

/* Waits until none of ims is loading, or timeout_ms. No event is delivered meanwhile. */
static bool wait_for(slate_image_t *const *ims, uint32_t n, zend_long timeout_ms)
{
#if SIZEOF_ZEND_LONG > 4   /* a 32-bit zend_long (the board) is at most 24 days: no overflow below */
    if (timeout_ms > INT64_C(1000000000000)) {
        timeout_ms = (zend_long)INT64_C(1000000000000);   /* 30 years: no overflow below */
    }
#endif
    int64_t deadline = bm_hal_now_us() + (int64_t)timeout_ms * 1000;
    for (;;) {
        uint32_t i = 0;
        while (i < n && slate_image_state(ims[i]) != SLATE_IMAGE_LOADING) {
            i++;
        }
        if (i == n) {
            return true;
        }
        int64_t left = deadline - bm_hal_now_us();
        if (left <= 0) {
            return false;
        }
        uint32_t ms = (uint32_t)((left + 999) / 1000);
        uint8_t t;
        bm_hal_queue_receive(s_wake, &t, ms < 20 ? ms : 20);   /* a token per finished image; 20 ms at most */
    }
}

static bool timeout_ok(zend_long ms)
{
    if (ms < 0) {
        zend_argument_value_error(1, "must be greater than or equal to 0");
        return false;
    }
    return true;
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_image_ctor, 0, 0, 3)
    ZEND_ARG_TYPE_MASK(0, x, MAY_BE_LONG | MAY_BE_DOUBLE, NULL)
    ZEND_ARG_TYPE_MASK(0, y, MAY_BE_LONG | MAY_BE_DOUBLE, NULL)
    ZEND_ARG_TYPE_INFO(0, src, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, opacity, IS_DOUBLE, 0, "1.0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_wait, 0, 0, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout_ms, IS_LONG, 0, "5000")
ZEND_END_ARG_INFO()

PHP_METHOD(SlateImage, __construct);

/* wait(): until the image is loaded (true) or failed (false), at most $timeout_ms; w, h and loaded follow. */
PHP_METHOD(SlateImage, wait)
{
    zend_long ms = 5000;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(ms)
    ZEND_PARSE_PARAMETERS_END();
    if (!timeout_ok(ms)) {
        RETURN_THROWS();
    }
    sl_obj_t *o = sl_obj(Z_OBJ_P(ZEND_THIS));
    if (!o->im) {
        RETURN_FALSE;
    }
    wait_for(&o->im, 1, ms);
    image_state(o);
    RETURN_BOOL(slate_image_state(o->im) == SLATE_IMAGE_READY);
}

static const zend_function_entry image_methods[] = {
    PHP_ME(SlateImage, __construct, arginfo_image_ctor, ZEND_ACC_PUBLIC)
    PHP_ME(SlateImage, wait, arginfo_wait, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

const zend_function_entry *sl_image_method_table(void)
{
    return image_methods;
}

/* ---- Images, Preload ---- */

static void preload_throw(path_check_t r, zend_string *p)
{
    switch (r) {
    case PATH_NOT_ABSOLUTE:
        zend_argument_value_error(1, "must hold absolute paths, \"%s\" is not one", ZSTR_VAL(p));
        break;
    case PATH_BAD_EXT:
        zend_argument_value_error(1, "must hold paths that end in .sli or .png, \"%s\" does not", ZSTR_VAL(p));
        break;
    case PATH_NO_PNG:
        png_not_built();
        break;
    case PATH_OK:
        break;
    }
}

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_images_preload, 0, 1, Baremetal\\Slate\\Preload, 0)
    ZEND_ARG_TYPE_INFO(0, paths, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* Images::preload(): every path checked first, then each image taken (loaded when it is not in memory). */
PHP_METHOD(SlateImages, preload)
{
    HashTable *paths;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY_HT(paths)
    ZEND_PARSE_PARAMETERS_END();
    zval *zv;
    ZEND_HASH_FOREACH_VAL(paths, zv) {
        ZVAL_DEREF(zv);
        if (Z_TYPE_P(zv) != IS_STRING) {
            zend_argument_type_error(1, "must contain only strings, %s given", zend_zval_value_name(zv));
            RETURN_THROWS();
        }
        path_check_t r = path_check(Z_STR_P(zv));
        if (r != PATH_OK) {
            preload_throw(r, Z_STR_P(zv));
            RETURN_THROWS();
        }
    } ZEND_HASH_FOREACH_END();
    if (!ready()) {
        RETURN_THROWS();
    }
    uint32_t n = zend_hash_num_elements(paths);
    object_init_ex(return_value, sl_preload_ce);
    sl_preload_t *pl = preload_of(Z_OBJ_P(return_value));
    pl->ims = n ? ecalloc(n, sizeof *pl->ims) : NULL;
    pl->flags = n ? ecalloc(n, 1) : NULL;
    ZEND_HASH_FOREACH_VAL(paths, zv) {
        ZVAL_DEREF(zv);
        slate_image_t *im = get(Z_STR_P(zv));
        if (!im) {
            zval_ptr_dtor(return_value);   /* puts what it took */
            ZVAL_UNDEF(return_value);
            RETURN_THROWS();
        }
        pl->ims[pl->n++] = im;
    } ZEND_HASH_FOREACH_END();
    zend_hash_index_update_ptr(&s_preloads, pl->std.handle, pl);
    preload_props(pl);
    for (uint32_t i = 0; i < pl->n; i++) {
        if (slate_image_state(pl->ims[i]) == SLATE_IMAGE_READY) {
            preload_mark(pl, i, false);   /* in memory: counted, no progress event */
            pl->flags[i] |= PL_ANNOUNCED;
            pl->ann++;
            pl->ann_ok++;
        }
    }
    if (pl->ann == pl->n) {
        emit_done(pl);   /* everything in memory: PreloadDone after the return to the event loop (or the idle hook's) */
    }
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_private_ctor, 0, 0, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(SlateImages, __construct)
{
    ZEND_PARSE_PARAMETERS_NONE();
}

static const zend_function_entry images_methods[] = {
    PHP_ME(SlateImages, __construct, arginfo_private_ctor, ZEND_ACC_PRIVATE)
    PHP_ME(SlateImages, preload, arginfo_images_preload, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

/* wait(): until no image of the preload is loading (true when all are loaded), at most $timeout_ms. */
PHP_METHOD(SlatePreload, wait)
{
    zend_long ms = 5000;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(ms)
    ZEND_PARSE_PARAMETERS_END();
    if (!timeout_ok(ms)) {
        RETURN_THROWS();
    }
    sl_preload_t *pl = preload_of(Z_OBJ_P(ZEND_THIS));
    if (pl->n > 0) {
        wait_for(pl->ims, pl->n, ms);
    }
    bool all = true;
    for (uint32_t i = 0; i < pl->n; i++) {
        preload_mark(pl, i, false);
        all = all && slate_image_state(pl->ims[i]) == SLATE_IMAGE_READY;
    }
    RETURN_BOOL(all);
}

PHP_METHOD(SlatePreload, __construct)
{
    ZEND_PARSE_PARAMETERS_NONE();
}

static const zend_function_entry preload_methods[] = {
    PHP_ME(SlatePreload, __construct, arginfo_private_ctor, ZEND_ACC_PRIVATE)
    PHP_ME(SlatePreload, wait, arginfo_wait, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static zend_object *preload_create(zend_class_entry *ce)
{
    sl_preload_t *pl = zend_object_alloc(sizeof(sl_preload_t), ce);
    memset(pl, 0, XtOffsetOf(sl_preload_t, std));
    zend_object_std_init(&pl->std, ce);
    object_properties_init(&pl->std, ce);
    pl->std.handlers = &preload_handlers;
    preload_props(pl);
    return &pl->std;
}

static void preload_free(zend_object *obj)
{
    sl_preload_t *pl = preload_of(obj);
    if (s_ready) {
        zend_hash_index_del(&s_preloads, obj->handle);
    }
    for (uint32_t i = 0; i < pl->n; i++) {
        slate_image_put(pl->ims[i]);
    }
    if (pl->ims) {
        efree(pl->ims);
    }
    if (pl->flags) {
        efree(pl->flags);
    }
    pl->n = 0;
    zend_object_std_dtor(obj);
}

/* ---- registration ---- */

static void ro(zend_class_entry *ce, const char *name, zend_type t)
{
    zval undef;
    ZVAL_UNDEF(&undef);
    zend_declare_typed_property(ce, zend_string_init_interned(name, strlen(name), 1), &undef,
                                ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL, t);
}

static zend_class_entry *event_class(const char *name)
{
    zend_class_entry ce;
    INIT_CLASS_ENTRY_EX(ce, name, strlen(name), NULL);
    zend_class_entry *c = zend_register_internal_class_ex(&ce, bm_events_base_ce());
    c->ce_flags |= ZEND_ACC_FINAL;
    return c;
}

static void error_case(const char *name, const char *value)
{
    zval v;
    ZVAL_STR(&v, zend_string_init_interned(value, strlen(value), 1));
    zend_enum_add_case_cstr(sl_image_error_ce, name, &v);
}

void sl_images_register(void)
{
    slate_images_set_listener(on_done, NULL);

    sl_image_error_ce = zend_register_internal_enum("Baremetal\\Slate\\ImageError", IS_STRING, NULL);
    error_case("NotFound", "not-found");
    error_case("Unreadable", "unreadable");
    error_case("BadFormat", "bad-format");
    error_case("Unsupported", "unsupported");
    error_case("NoMemory", "no-memory");

    zend_type lng = (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 0, 0);
    zend_type bln = (zend_type)ZEND_TYPE_INIT_CODE(_IS_BOOL, 0, 0);
    zend_type str = (zend_type)ZEND_TYPE_INIT_CODE(IS_STRING, 0, 0);
    zend_type arr = (zend_type)ZEND_TYPE_INIT_CODE(IS_ARRAY, 0, 0);

    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Images", images_methods);
    sl_images_ce = zend_register_internal_class(&ce);
    sl_images_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE;

    memcpy(&preload_handlers, zend_get_std_object_handlers(), sizeof preload_handlers);
    preload_handlers.offset = XtOffsetOf(sl_preload_t, std);
    preload_handlers.free_obj = preload_free;
    preload_handlers.clone_obj = NULL;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Slate", "Preload", preload_methods);
    sl_preload_ce = zend_register_internal_class(&ce);
    sl_preload_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE;
    sl_preload_ce->create_object = preload_create;
    ro(sl_preload_ce, "loaded", lng);
    ro(sl_preload_ce, "total", lng);
    ro(sl_preload_ce, "done", bln);

    if (!bm_events_base_ce()) {
        return;   /* ext-events is a required module: never the case */
    }
    sl_image_loaded_ce = event_class("Baremetal\\Slate\\ImageLoaded");
    ro(sl_image_loaded_ce, "path", str);
    ro(sl_image_loaded_ce, "w", lng);
    ro(sl_image_loaded_ce, "h", lng);
    sl_image_failed_ce = event_class("Baremetal\\Slate\\ImageFailed");
    ro(sl_image_failed_ce, "path", str);
    ro(sl_image_failed_ce, "error", (zend_type)ZEND_TYPE_INIT_CLASS(sl_image_error_ce->name, 0, 0));
    ro(sl_image_failed_ce, "message", str);
    sl_preload_progress_ce = event_class("Baremetal\\Slate\\PreloadProgress");
    ro(sl_preload_progress_ce, "loaded", lng);
    ro(sl_preload_progress_ce, "total", lng);
    sl_preload_done_ce = event_class("Baremetal\\Slate\\PreloadDone");
    ro(sl_preload_done_ce, "failed", arr);
}
