/* Slate's image store and loader task: see images.h. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "bm_events_hal.h"
#include "images.h"
#include "slate_config.h"
#if SLATE_HAS_PNG
#include "png.h"
#endif

#define SLI_HEADER 16

struct slate_image {
    struct slate_image *next;    /* the store (lock) */
    struct slate_image *qnext;   /* the loader's queue (lock) */
    uint32_t hash;               /* of path */
    uint32_t refs;               /* lock; the loader holds one while the image is queued or loading */
    uint8_t state;               /* slate_image_state_t, lock */
    uint8_t err;                 /* slate_img_err_t, set with state */
    bool linked;                 /* in the store (lock); a failed image leaves it before its last put */
    void (*release)(void *);     /* frees this record and data */
    uint8_t *data;               /* the .sli file (READY) */
    slate_sli_t sli;             /* READY: set before state, never changed after */
    char path[];
};

static void *ext_alloc(size_t n)
{
    return bm_hal_alloc(n, BM_HAL_MEM_EXTERNAL);
}

static bm_hal_lock_t *s_lock;            /* created by the first get (PHP task), never freed */
static slate_image_t *s_all;             /* the store */
static slate_image_t *s_qhead, *s_qtail; /* requests not yet taken by the loader */
static bool s_running;                   /* the loader task runs (it ends with s_running cleared, under the lock) */
static uint32_t s_done;
static void (*s_listener)(slate_image_t *im, void *arg);
static void *s_listener_arg;
static void *(*s_alloc)(size_t) = ext_alloc;
static void (*s_free)(void *) = bm_hal_free;
static bool (*s_reclaim)(void);          /* slate_images_set_reclaim (atomic) */
static void *(*s_try_alloc)(size_t);     /* loader task only: the allocator of the image being read */
static bool s_alloc_failed;              /*   it refused a block while reading it */

/* The loader's allocator: the store's, noting a refusal (only a refused block asks for a reclaim: a size past
   SIZE_MAX is NO_MEMORY too, and no memory freed changes it). */
static void *loader_alloc(size_t n)
{
    void *p = s_try_alloc(n);
    if (!p) {
        s_alloc_failed = true;
    }
    return p;
}

/* s_lock is created by the first get on the PHP task and read by every task (the engine at each wake-up): stored
   with release, loaded with acquire, so another core sees it whole (it lives in internal RAM: atomics are fine). No
   lock yet means no image yet: nothing to protect. */
static bm_hal_lock_t *the_lock(void)
{
    return __atomic_load_n(&s_lock, __ATOMIC_ACQUIRE);
}

/* lock() returns the lock it entered (NULL: none yet) and unlock() leaves that one: a lock created by the PHP task
   on the other core between the two is never left without having been entered. */
static bm_hal_lock_t *lock(void)
{
    bm_hal_lock_t *l = the_lock();
    if (l) {
        bm_hal_lock_enter(l);
    }
    return l;
}

static void unlock(bm_hal_lock_t *l)
{
    if (l) {
        bm_hal_lock_exit(l);
    }
}

static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    return h;
}

/* lock held */
static void unlink_image(slate_image_t *im)
{
    if (!im->linked) {
        return;
    }
    im->linked = false;
    for (slate_image_t **p = &s_all; *p; p = &(*p)->next) {
        if (*p == im) {
            *p = im->next;
            return;
        }
    }
}

/* no lock: im is out of the store and nobody holds it */
static void destroy(slate_image_t *im)
{
    if (im->data) {
        im->release(im->data);
    }
    im->release(im);
}

/* ---- reading a file ---- */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* path ends in ext (lower case), in any case: ".PNG" and ".Png" are PNG files, as phpbm takes them */
static bool ends_with(const char *path, const char *ext)
{
    size_t n = strlen(path), k = strlen(ext);
    if (n < k) {
        return false;
    }
    for (size_t i = 0; i < k; i++) {
        char c = path[n - k + i];
        if ((c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c) != ext[i]) {
            return false;
        }
    }
    return true;
}

/* A .sli file as it is: the header, then exactly the size it gives. */
static slate_img_err_t read_sli(FILE *f, void *(*alloc)(size_t), void (*release)(void *), uint8_t **data,
                                size_t *size)
{
    uint8_t hdr[SLI_HEADER];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        return ferror(f) ? SLATE_IMG_UNREADABLE : SLATE_IMG_BAD_FORMAT;
    }
    unsigned w = hdr[6] | (unsigned)hdr[7] << 8, h = hdr[8] | (unsigned)hdr[9] << 8;
    uint64_t dlen = rd32(hdr + 12);
    if (memcmp(hdr, "SLI1", 4) != 0 || hdr[4] != 1 || w < 1 || w > 32767 || h < 1 || h > 32767) {
        return SLATE_IMG_BAD_FORMAT;
    }
    /* the most a row can take: 5 bytes per pixel (a one-pixel semi-transparent command with its padding byte) and
       3 bytes to align the next row; a larger size is not a .sli (and is not allocated) */
    if (dlen > (uint64_t)h * ((uint64_t)w * 5 + 3)) {
        return SLATE_IMG_BAD_FORMAT;
    }
    uint64_t total = SLI_HEADER + 4 * (uint64_t)h + dlen;
    if (total > SIZE_MAX) {
        return SLATE_IMG_NO_MEMORY;
    }
    uint8_t *d = alloc((size_t)total);
    if (!d) {
        return SLATE_IMG_NO_MEMORY;
    }
    memcpy(d, hdr, sizeof hdr);
    size_t rest = (size_t)total - sizeof hdr;
    slate_img_err_t err = SLATE_IMG_OK;
    uint8_t extra;
    if (fread(d + sizeof hdr, 1, rest, f) != rest) {
        err = ferror(f) ? SLATE_IMG_UNREADABLE : SLATE_IMG_BAD_FORMAT;
    } else if (fread(&extra, 1, 1, f) != 0) {
        err = SLATE_IMG_BAD_FORMAT;   /* bytes after the size the header gives */
    } else if (ferror(f)) {
        err = SLATE_IMG_UNREADABLE;
    }
    if (err != SLATE_IMG_OK) {
        release(d);
        return err;
    }
    *data = d;
    *size = (size_t)total;
    return SLATE_IMG_OK;
}

#if SLATE_HAS_PNG
static int png_read(void *ctx, uint8_t *buf, size_t n)
{
    FILE *f = ctx;
    size_t k = fread(buf, 1, n, f);
    return k == 0 && ferror(f) ? -1 : (int)k;
}
#endif

slate_img_err_t slate_image_read(const char *path, void *(*alloc)(size_t), void (*release)(void *), uint8_t **data,
                                 size_t *size)
{
    *data = NULL;
    *size = 0;
    bool png = ends_with(path, ".png");
#if !SLATE_HAS_PNG
    if (png) {
        return SLATE_IMG_UNSUPPORTED;
    }
#endif
    errno = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return errno == ENOENT || errno == ENOTDIR ? SLATE_IMG_NOT_FOUND : SLATE_IMG_UNREADABLE;
    }
    setvbuf(f, NULL, _IONBF, 0);   /* no stdio buffer: .sli is read in two big reads, PNG has its own */
    slate_img_err_t err;
#if SLATE_HAS_PNG
    if (png) {
        err = slate_png_to_sli(png_read, f, alloc, release, data, size);
    } else
#endif
    {
        err = read_sli(f, alloc, release, data, size);
    }
    fclose(f);
    return err;
}

/* ---- the loader task ---- */

/* The end of a loading: the state, then the listener (unless the loader's is the only reference left). */
static void finish(slate_image_t *im, slate_img_err_t err, uint8_t *data, const slate_sli_t *sli)
{
    bm_hal_lock_t *lk = lock();
    im->data = data;
    if (err == SLATE_IMG_OK) {
        im->sli = *sli;
    }
    im->err = (uint8_t)err;
    im->state = err == SLATE_IMG_OK ? SLATE_IMAGE_READY : SLATE_IMAGE_FAILED;
    s_done++;
    bool alone = im->refs == 1;
    if (alone || err != SLATE_IMG_OK) {
        unlink_image(im);   /* a failed image leaves the store: the next get of its path loads it again */
    }
    if (alone) {
        im->refs = 0;
    }
    void (*listener)(slate_image_t *, void *) = s_listener;
    void *arg = s_listener_arg;
    unlock(lk);
    if (alone) {
        destroy(im);   /* nobody wants it any more */
        return;
    }
    if (listener) {
        listener(im, arg);
    }
    slate_image_put(im);   /* the loader's reference */
}

static void loader_main(void *arg)
{
    (void)arg;
    for (;;) {
        bm_hal_lock_t *lk = lock();
        slate_image_t *im = s_qhead;
        if (!im) {
            s_running = false;   /* the next request starts a new task */
            unlock(lk);
            return;
        }
        s_qhead = im->qnext;
        if (!s_qhead) {
            s_qtail = NULL;
        }
        im->qnext = NULL;
        bool dropped = im->refs == 1;
        if (dropped) {
            unlink_image(im);
            im->refs = 0;
        }
        void *(*alloc)(size_t) = s_alloc;
        unlock(lk);
        if (dropped) {
            destroy(im);   /* put down while it waited: not loaded */
            continue;
        }
        uint8_t *data = NULL;
        size_t size = 0;
        slate_sli_t sli;
        slate_img_err_t err = SLATE_IMG_OK;
        s_try_alloc = alloc;
        for (int attempt = 0; attempt < 2; attempt++) {
            s_alloc_failed = false;
            err = slate_image_read(im->path, loader_alloc, im->release, &data, &size);
            if (err == SLATE_IMG_OK && !slate_sli_open(&sli, data, size)) {
                err = SLATE_IMG_BAD_FORMAT;
            }
            if (err != SLATE_IMG_OK && data) {
                im->release(data);
                data = NULL;
            }
            /* a block refused: the caches of the scenes left go (the engine drops them on its task), and when that
               freed something, once more */
            bool (*reclaim)(void) = __atomic_load_n(&s_reclaim, __ATOMIC_ACQUIRE);
            if (err != SLATE_IMG_NO_MEMORY || !s_alloc_failed || attempt > 0 || !reclaim || !reclaim()) {
                break;
            }
        }
        finish(im, err, data, &sli);
    }
}

/* ---- the store ---- */

slate_image_t *slate_image_get(const char *path)
{
    if (!path) {
        return NULL;
    }
    if (!the_lock()) {
        bm_hal_lock_t *l = bm_hal_lock_create();   /* only the PHP task gets here: no other creator */
        if (!l) {
            return NULL;
        }
        __atomic_store_n(&s_lock, l, __ATOMIC_RELEASE);
    }
    uint32_t h = hash(path);
    bm_hal_lock_t *lk = lock();
    slate_image_t *im = s_all;
    while (im && !(im->hash == h && strcmp(im->path, path) == 0)) {
        im = im->next;
    }
    if (im) {
        im->refs++;
    }
    unlock(lk);
    if (im) {
        return im;
    }
    size_t len = strlen(path);
    im = s_alloc(sizeof *im + len + 1);
    if (!im) {
        return NULL;
    }
    memset(im, 0, sizeof *im);
    memcpy(im->path, path, len + 1);
    im->hash = h;
    im->refs = 2;   /* the caller's and the loader's */
    im->state = SLATE_IMAGE_LOADING;
    im->release = s_free;
    lk = lock();
    im->next = s_all;
    s_all = im;
    im->linked = true;
    if (s_qtail) {
        s_qtail->qnext = im;
    } else {
        s_qhead = im;
    }
    s_qtail = im;
    bool start = !s_running;
    s_running = true;
    unlock(lk);
    if (start && !bm_hal_task_start(loader_main, NULL, "slate-img", SLATE_LOADER_STACK, SLATE_LOADER_PRIORITY,
                                    BM_HAL_CORE_ANY)) {
        /* no task to load them: everything queued fails (only this image: the queue is empty when no task runs) */
        lk = lock();
        void (*listener)(slate_image_t *, void *) = s_listener;
        void *larg = s_listener_arg;
        slate_image_t *q = s_qhead, *gone = NULL;
        s_qhead = s_qtail = NULL;
        s_running = false;
        while (q) {
            slate_image_t *next = q->qnext;
            q->qnext = NULL;
            q->err = SLATE_IMG_NO_MEMORY;
            q->state = SLATE_IMAGE_FAILED;
            s_done++;
            unlink_image(q);   /* failed: out of the store, the next get tries again */
            if (--q->refs == 0) {
                q->qnext = gone;
                gone = q;
            }
            q = next;
        }
        unlock(lk);
        while (gone) {
            slate_image_t *next = gone->qnext;
            destroy(gone);
            gone = next;
        }
        if (listener) {
            listener(im, larg);   /* as for any failure, once; im is alive: the caller's reference */
        }
    }
    return im;
}

void slate_image_ref(slate_image_t *im)
{
    if (im) {
        bm_hal_lock_t *lk = lock();
        im->refs++;
        unlock(lk);
    }
}

void slate_image_put(slate_image_t *im)
{
    if (!im) {
        return;
    }
    bm_hal_lock_t *lk = lock();
    bool last = --im->refs == 0;
    if (last) {
        unlink_image(im);
    }
    unlock(lk);
    if (last) {
        destroy(im);
    }
}

slate_image_state_t slate_image_state(const slate_image_t *im)
{
    bm_hal_lock_t *lk = lock();
    slate_image_state_t st = (slate_image_state_t)im->state;
    unlock(lk);
    return st;
}

const slate_sli_t *slate_image_sli(const slate_image_t *im)
{
    return slate_image_state(im) == SLATE_IMAGE_READY ? &im->sli : NULL;
}

slate_img_err_t slate_image_error(const slate_image_t *im)
{
    bm_hal_lock_t *lk = lock();
    slate_img_err_t err = im->state == SLATE_IMAGE_FAILED ? (slate_img_err_t)im->err : SLATE_IMG_OK;
    unlock(lk);
    return err;
}

const char *slate_image_path(const slate_image_t *im)
{
    return im->path;
}

void slate_images_set_listener(void (*done)(slate_image_t *im, void *arg), void *arg)
{
    bm_hal_lock_t *lk = lock();
    s_listener = done;
    s_listener_arg = arg;
    unlock(lk);
}

uint32_t slate_images_done(void)
{
    bm_hal_lock_t *lk = lock();
    uint32_t n = s_done;
    unlock(lk);
    return n;
}

bool slate_images_loading(void)
{
    bm_hal_lock_t *lk = lock();
    bool r = s_running;
    unlock(lk);
    return r;
}

void slate_images_set_reclaim(bool (*reclaim)(void))
{
    __atomic_store_n(&s_reclaim, reclaim, __ATOMIC_RELEASE);
}

void slate_images_set_alloc(void *(*alloc)(size_t bytes), void (*free_fn)(void *p))
{
    s_alloc = alloc && free_fn ? alloc : ext_alloc;
    s_free = alloc && free_fn ? free_fn : bm_hal_free;
}
