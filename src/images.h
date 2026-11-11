/* images.h - Slate's image store: images shared by path, counted references, and the loader task that reads them.
   Pure C, no ESP-IDF and no PHP: files through the C standard I/O, the task and the lock from the events HAL of
   ext-events.

   Store. One image per path: slate_image_get of a path that is in the store (loading or ready) takes another
   reference to that image and loads nothing; a new path makes a new image, LOADING, queued for the loader. An image
   that fails leaves the store at once: its holders keep the FAILED image, and the next get of that path makes a new
   one and loads the file again (a card mounted later, a file written later, memory available again).
   An image goes when its last reference does (slate_image_put, from any task): the image elements of the scene,
   the commands that carry it and the PHP objects each hold one. A path whose image went is loaded again by the
   next get.

   States. LOADING -> READY (slate_image_sli: the .sli file, checked by slate_sli_open; immutable from then on, so
   the compositor reads it without a lock) or FAILED (slate_image_error). The change happens once, under the
   store's lock, and is never undone.

   Loader task. Started by the first request (bm_hal_task_start: SLATE_LOADER_STACK bytes of stack, priority
   SLATE_LOADER_PRIORITY relative to the PHP task, any core); it takes the requests one at a time, in the order they
   came, and ends when the queue is empty: its stack exists only while there is something to load. For each image:
   the file is opened with fopen (no such file or directory: NOT_FOUND; any other failure: UNREADABLE) and read
   without a stdio buffer; a path ending in ".png" (in any case: ".PNG" too, as phpbm takes it; the PHP layer's
   extension check must match without regard to case as well) is decoded to .sli by slate_png_to_sli (png.h; UNSUPPORTED when
   the png format is not built), any other path is read as a .sli file as it is (its header first, then exactly the
   size it gives: a short file or a byte more is BAD_FORMAT, a read error UNREADABLE), then slate_sli_open checks it
   (BAD_FORMAT when it refuses). Memory: NO_MEMORY. While the loader works on an image it holds a reference of its
   own: an image put down to that reference during its loading is freed when the loading ends; one put down to it
   while it waits in the queue is freed without being loaded; in both cases the listener is not called.
   When an image becomes READY or FAILED in the task, the listener is called from the task with the image (alive
   for the call; take a reference to keep it): the PHP layer raises its events and kicks the engine
   (slate_engine_kick), which marks dirty the elements that show it (engine.c, slate_images_done). The listener
   must not block for long (the next image waits) and must not call into PHP.
   When the task cannot be started (no memory), the image comes back from slate_image_get already FAILED with
   NO_MEMORY, and the listener is called for it from slate_image_get (the PHP task) before it returns: every
   failure and every load is told exactly once, always by the store.

   Memory: the image record and its file from the store's allocator (default: bm_hal_alloc BM_HAL_MEM_EXTERNAL,
   PSRAM on the board), outside the element cache budget. A PNG is decoded row by row (png.h), never whole.
   Threads: slate_image_get and the setters from the PHP task; slate_image_ref, slate_image_put and the getters from
   any task (not an ISR); the lock is the events HAL's short critical section, never held across an allocation, a
   free, a file access or the listener. */
#ifndef SLATE_IMAGES_H
#define SLATE_IMAGES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sli.h"

#ifndef SLATE_LOADER_STACK
/* bytes of internal RAM, only while the task runs. The work of one image measured on the PC (images_test, x86-64,
   glibc's fopen / fread included): 0.7 KB for a .sli, up to 2.0 KB for a PNG (the PLTE buffer, inflate, the
   encoder); about 3 KB more for the windowed Xtensa frames, the task's saved context and ESP-IDF's path from fread
   to the card or flash (VFS, FATFS with its long-name buffer on the heap, the SD or wear-levelling driver).
   Measured on an ESP32-S3 (high-water mark, the listener included): 2.1 KB for a .sli from flash, 3.0 KB from the
   microSD, 2.8 KB for a PNG from flash and 3.8 KB for a PNG from the microSD. 6 KB leaves about 2.3 KB for paths
   not measured there, such as a driver logging a read error. */
#define SLATE_LOADER_STACK 6144
#endif
#ifndef SLATE_LOADER_PRIORITY
#define SLATE_LOADER_PRIORITY (-1)   /* below the PHP task (and the engine) */
#endif

typedef struct slate_image slate_image_t;   /* shared, refcounted; immutable once READY */
typedef enum { SLATE_IMAGE_LOADING, SLATE_IMAGE_READY, SLATE_IMAGE_FAILED } slate_image_state_t;

/* PHP task: the image of path (taken: refcount + 1); a new one is queued for loading. NULL: no memory for the
   record (or no lock). path: as given (the PHP layer checks that it is absolute and ends in .sli or .png). */
slate_image_t *slate_image_get(const char *path);
/* Another reference to im (refcount + 1). Any task. NULL allowed. */
void slate_image_ref(slate_image_t *im);
/* refcount - 1; freed at 0 (any task). NULL allowed. */
void slate_image_put(slate_image_t *im);
slate_image_state_t slate_image_state(const slate_image_t *im);
const slate_sli_t *slate_image_sli(const slate_image_t *im);      /* READY only (NULL otherwise) */
slate_img_err_t slate_image_error(const slate_image_t *im);       /* FAILED only (SLATE_IMG_OK otherwise) */
const char *slate_image_path(const slate_image_t *im);            /* as given to get */

/* Called by the loader task when an image is READY or FAILED: the engine is kicked and the PHP layer raises its
   events. Set once by the PHP layer (Task 8) before the first get and never cleared or replaced: the loader copies
   the pair under the lock and calls it after, so a call may still run with the old pair after a change, and arg
   must live as long as the program. */
void slate_images_set_listener(void (*done)(slate_image_t *im, void *arg), void *arg);

/* Images that became READY or FAILED so far (wraps): the engine compares it with what it saw to know when to look
   at its image elements again. Any task. */
uint32_t slate_images_done(void);
/* The loader task runs (requests are queued or being loaded). Any task. */
bool slate_images_loading(void);

/* The store's memory (NULL, NULL: the default, external RAM). Only while no image lives (tests). */
void slate_images_set_alloc(void *(*alloc)(size_t bytes), void (*free_fn)(void *p));

/* What the loader does with one file, at once in the calling task: reads path (a .png decoded) into a .sli file
   from alloc (*data, *size; give it back with release). The file is not checked: slate_sli_open does it. Any error:
   *data NULL, *size 0, nothing left allocated. */
slate_img_err_t slate_image_read(const char *path, void *(*alloc)(size_t), void (*release)(void *), uint8_t **data,
                                 size_t *size);

#endif
