---
eyebrow: 'ext-slate · PHP API'
lede:    'What a frame costs and how to keep it short: only the dirty part is composed, opaque elements hide what is under them, and caches keep shadows, blur and heavy groups as images.'
see_also:
  - { href: './debug.md', meta: 'API' }
  - { href: './effects-and-options.md', meta: 'API' }
prev: { label: 'Effects and build options', href: './effects-and-options.md' }
next: { label: 'Debug', href: './debug.md' }
---

# Performance

## How a frame is made

1. The changes a listener made are handed to the compositor together when PHP returns to the event loop.
2. The compositor applies them and computes the dirty area: for every change, the screen box of the element
   before and after it (shadows and anti-aliased edges included). Up to 32 rectangles are kept; two are merged
   only when their common box costs fewer extra pixels than a separate region (about 1000), so changes far apart
   (a dozen dots moving across the screen) stay small separate regions instead of one large rectangle. A change
   of an `Arc`'s `$from` or `$to` marks only the part of the ring between the old and the new end, with its caps:
   a gauge that moves by a few degrees redraws a few hundred pixels, not the whole arc. A `RoundRect` or `Circle`
   with a `$shadow` sits on whole pixels: a move that leaves it on the same pixel changes nothing. A property set
   to the value it already has changes nothing, and `set(x: .., y: ..)` is one move: the box before and the box
   after, not the one in between.
3. At the next TE pulse of the panel (or at its refresh rate without TE) it composes only those rectangles, in
   bands of rows: while the panel receives one band, the next one is drawn. Bands live in internal RAM; a region
   narrower than the screen takes as many rows as a band buffer holds. What is under the changes is copied from the
   underlay when there is one (see below) instead of being drawn again, except where an opaque element covers it.
4. Nothing changed, nothing composed: a still screen costs no frame, no band and no wake-up of the compositor.

At most one frame per TE pulse: changes made faster than the panel refreshes are merged into the next frame.

## No tearing

A panel such as the ST77916 refreshes its picture from its own memory, top to bottom, once per TE period (16.7 ms at
60 Hz). A band written while that refresh passes over it shows split for one refresh: a moving element tears. With a
TE pin, the compositor measures the period from the pulses and knows where the refresh is at any moment; before
sending a band it checks that the refresh will not cross it while it is written, and otherwise waits until the
refresh has passed it. Each band appears whole, in one refresh.

Every band of a frame goes behind the refresh that the frame's pulse started, never ahead of it, so the whole frame
appears in the next refresh, wherever its bands are on the screen. A band that went ahead would appear one refresh
earlier than the others, and whether it can depends on its row and on how long the bands before it took: an element
moving across the screen would then skip or repeat a step now and then. The cost is a steady delay of one refresh
between the pulse that starts a frame and the refresh that shows it.

- It applies to frames that change at most half of the screen, which is where tearing shows (a moving pointer, a
  dot, a gauge). A larger frame takes about a refresh or more to send, so a refresh shows it partly drawn whatever
  the order; it is sent as before, as fast as it is composed.
- While a band waits for the refresh, the compositor draws the next band into the other band buffer; it sleeps
  only when both wait, and that counts as `wait` in the debug line. A frame whose last band is
  near the bottom of the screen ends just after the next pulse (it waits for the refresh to pass the bottom rows);
  the next frame then starts at once.
- Without a TE pin, or with a panel driver that does not tell the order of its refresh, frames are sent as they
  are composed.

## What costs

| Work | Cost |
| --- | --- |
| Opaque fills (a `RoundRect` or `Circle` with an opaque color and opacity 1, a `Canvas`) | Cheapest: whole spans are written as they are; only the anti-aliased edge pixels are blended. Opaque elements also hide what is under them: an element fully covered by an opaque one above it is not drawn at all, nor the background. |
| Alpha (a color with alpha, `$opacity` below 1) | Every covered pixel is blended with what is under it, and nothing under it can be skipped. |
| A group with `$opacity` below 1 | Composed apart (in strips, in internal RAM), then blended as a whole. The strips come from a scratch area of internal RAM that the Slate takes when such a group is first drawn, as much as the group needs (3 bytes per pixel of two strips of its width, at most a band's worth: 43 KB on a 360-wide panel), and gives back when no frame has needed all of it for a second, or another scene is shown: the rest of the program, Wi-Fi for one, has that RAM meanwhile. When internal RAM is short it takes less, which means more, smaller strips. |
| Gradients | A color is computed for every pixel (integer steps and a dither). |
| `Text` | Proportional to the glyph pixels; anti-aliased fonts (2 or 4 bits per pixel) blend their edges. |
| Shadows and blur | Expensive to build (a blur of the image), cheap to reuse: they are kept in a cache and rebuilt only when what they depend on changes. |
| `Image` | An opaque `.sli` image at opacity 1 is copied run by run (a run of one color is a fill) and hides what is under it, like an opaque fill: a full-screen background image costs about what a copy of the screen costs. Transparent runs are skipped; semi-transparent pixels (anti-aliased edges, shadows drawn into the image) are blended; at an `$opacity` below 1 every pixel is blended. The image is read from external RAM in order, as a cache is. |
| `Canvas::dirty()` | The changed rectangle is copied to the compositor; send small rectangles when only a part changed. A canvas lives twice, in PHP memory and in the compositor's (2 bytes per pixel each, at most 2 MB for 1024 x 1024), plus a copy of the rectangle in flight at each `dirty()`. |

So: prefer opaque backgrounds for large areas, keep translucent elements small, and give heavy content a cache.

## Caches

A `Group` with `$cache`, a `$shadow` or a `$blur`, and a `RoundRect` or `Circle` with a `$shadow`, are drawn
through images kept in external RAM (PSRAM):

- moving the element, changing its opacity or its visibility reuses the image;
- a change of something inside (any property of a child, a child added or removed) rebuilds it at the next
  frame that needs it.

When another scene is shown, the caches of the scene left stay while there is room (`Slate::$keepScenes`, default
2 scenes, and `Slate::$keptBudget`; see [Returning to a scene](./scenes.md#returning-to-a-scene)): showing it again
costs no build. They stay in the budget below, and they are the first to go when the scene shown or the underlay
needs room there, the scene left first before the others, and when an image cannot be loaded for lack of memory
(see [Images](./images.md#memory)); a scene whose caches went builds its shadows and blurs again at its first
frame. Keeping them costs no work per frame; with `$keepScenes = 0` (or `Scene::$keepCache =
false`) the caches of a scene go when it is left, as in 1.1.

A change of anything inside a group with `$cache`, a `$shadow` or a `$blur` redraws the group's whole box, since
its image is built again.

Give `$cache` to a group that is drawn often but changes rarely (a dial behind a moving needle, a static card),
not to one whose content changes every frame: it would be rebuilt every frame.

Cached elements sit on whole pixels: their position is rounded to the nearest pixel, so an animation that moves
them by fractions of a pixel shifts the image instead of rebuilding it. Elements without a cache keep their
sub-pixel position and anti-aliasing.

The caches together stay under a budget: half of the largest block of external RAM found when the Slate starts,
at most 2 MB. Past it, the elements that do not fit are drawn directly, and a group drawn that way loses its
blur and shadow until memory frees up. On a board without PSRAM the budget is 0: nothing is cached, shadows and
group blur are not drawn, and internal RAM stays free for the bands, Wi-Fi and DMA. Images are outside this budget but
in the same external RAM, the part the SDK leaves outside PHP (on the esp32 SDK, the `firmware-reserve` option):
see [Images](./images.md#memory).

## Images

An image is read from external RAM by the band that needs its rows, and only the runs inside the part being
redrawn are drawn. A still image is part of the underlay like other elements; an opaque one at opacity 1 counts
as cheap (a copy), so a full-screen background image alone does not make an underlay. When an image finishes
loading, only its box is composed again; until then it draws and covers nothing.

Loading happens in a task of low priority, one image at a time: PHP and the frames go on meanwhile. A `.sli` is
read as it is: its load time is the time to read the file (from flash, or from the microSD card). A PNG decoded on
the board costs much more time and, for a moment, about twice its `.sli` size in memory (see
[Images](./images.md#memory)): convert the project's images with phpbm (the assets of `composer.json`) and keep
on-board PNG for files that arrive otherwise. To show a page without waiting for its images, preload them while
the page before is shown (`Images::preload()`).

### Loading times

Measured on an ESP32-S3 at 240 MHz with PSRAM, the microSD card on an SPI bus at 20 MHz (the board of the
`06-display/slate-images` example), the time the loading task takes for one image:

| Image | From flash | From the microSD |
| --- | --- | --- |
| `.sli`, an icon of 72 x 72 (8.6 KB) | 2.4 ms | not measured |
| `.sli`, a photo of 360 x 360 (241 KB) | 24.5 ms (about 10 MB/s) | 168 ms (about 1.4 MB/s) |
| PNG, an RGBA icon of 72 x 72 (3.7 KB) | 11.2 ms | 16.9 ms |
| PNG, an RGBA picture of 160 x 100 (5.0 KB) | not measured | 35.8 ms |
| PNG, an RGB photo of 360 x 360 (39.9 KB) | 303 ms | 348 ms |

A `.sli` costs the time to read its bytes. A PNG costs its decoding, about 2.2 µs per pixel whatever its
compression (from 3 to 8 ms per KB of PNG file for these images), plus the read. A `wait()` returns 2 to 5 ms
after the image is ready.

What this means for a page change, with a 360 x 360 photo from the microSD as background and an icon from flash:

| | From `show()` to the complete page |
| --- | --- |
| Images not in memory | about 200 ms: the first frame (23 ms) shows the page without them, a second frame adds them when they are loaded |
| Images preloaded (`Images::preload()`) | 27 ms: one frame of 20.5 ms, with everything |

A full-screen opaque `.sli` with an icon and a line of text over it takes about 20 ms to compose. Elements moving
over a still photo are cheap: with a 360 x 360 photo, four icons, six translucent dots and an icon moving at
60 Hz, the frames come at 60 per second (frame 14 ms, composing 4.2 ms, medians over 20 s).

## The underlay

The elements at the bottom of the scene that have not changed for a couple of frames (a full-screen gradient, a
dial, labels: the run from the bottom up to the first one that moves) are kept, drawn on the background color, as
one screen-sized image in external RAM, the underlay. A frame then copies the underlay under the parts that
changed and draws only the elements above it: a radial gradient behind moving elements is not computed again in
every frame. The pixels are the same as when everything is drawn.

- It is made piece by piece, from the parts the frames draw anyway, so it adds no slow frame.
- A change to one of those bottom elements, another scene or another background color starts it again.
- It is made only when the bottom holds something that costs more to draw than to copy (a gradient, an `Arc`,
  a `Line`, `Text`, a border); a plain colored background is drawn directly.
- It takes 2 bytes per pixel of the screen (about 270 KB on a 360 x 360 panel) from the caches' budget, and it
  gives the memory back as soon as an element's cache needs it: caches come first. The kept caches of the scenes
  left go before it.
- Elements drawn through an image (a `Group` with `$cache`, `$shadow`, `$blur` or `$opacity` below 1, a shape
  with a `$shadow`) end the run: the underlay holds only what is drawn directly.
- During a transition between scenes it is not used and its memory is given back; it is made again for the new
  scene after the end.

## One Slate at a time

The compositor and its rasterizers are a single instance: one Slate runs at a time in the firmware, whatever
the number of panels. It runs in its own task, on core 0 (PHP runs on core 1); a listener that keeps PHP busy
does not delay the frames already handed over, and a long frame does not block PHP. Core 0 was chosen by
measuring on an ESP32-S3: with the compositor on PHP's core the busy scene below drops from about 55 to about 42
frames per second, and the gauge example has almost four times as many uneven steps.

## Full-screen times

What a whole new screen costs (a `show()` of a scene that fills a 360 x 360 panel), measured with `slate-debug`
on an ESP32-S3 at 240 MHz with a 32 KB instruction cache, with PSRAM, on a QSPI panel (ST77916): the frame time is
the composing of the bands plus the wait for the panel. About 12.5 ms is the floor, the time to send a whole screen
to the panel: below it the drawing hides behind the transfer.

| Scene | Frame | Of which composing |
| --- | --- | --- |
| One opaque `RoundRect` | 12.6 ms | 4.7 ms |
| A full-screen `RoundRect` at half alpha over an opaque one | 14.5 ms | 13.2 ms |
| A full-screen `LinearGradient` (two stops) | 12.9 ms | 5.5 ms |
| A `Circle` of radius 180 with a `RadialGradient` | 42 to 43 ms | 42 to 43 ms |
| A `Circle` of radius 170 | 13.2 ms | 7.2 ms |
| A `RoundRect` 320 x 320 with radius 40 | 12.9 ms | 5.9 ms |
| 16 lines of `Text` | 13.3 ms | 10.2 ms |
| An `Arc` of radius 150, width 20 | 13.4 ms | 10.2 ms |
| A 240 x 240 `RoundRect` with `Shadow(0, 6, 14, 0xA0000000)`, first frame (the shadow is built) | 31.5 ms | 28.8 ms |
| A full-screen `Group` with `opacity: 0.6` (a `RoundRect` and a `Circle` inside) | 50.3 ms | 50.1 ms |
| A 240 x 240 `Group` with `blur: 8`, first frame | 135 ms | 135 ms |
| A 240 x 240 `Group` with a `Shadow`, first frame | 60 to 125 ms | 60 to 125 ms |

The pixel code of Slate is compiled for speed even when the firmware is built for size. A radial gradient
costs most of the shapes because its rows are cut into short pieces, each with its own square roots; keep large
radial fills for screens that are not redrawn often. The blur and the shadow of a group are built once, at the
first frame, and reused when the group moves; the time of a group's shadow depends on where its images land in
external RAM, and changes from one firmware build to the next. The `slate-bench` example runs these scenes.

## A busy scene

Twelve translucent dots orbiting at different speeds, a gradient card with a shadow sliding left and right, a group
whose opacity pulses, a progress ring and a frame counter, all changed every 1/60 s by a PHP timer, over a
full-screen `RadialGradient` and a ring track; the IMU polled at 100 Hz and Wi-Fi connecting at the same time
(ESP32-S3 at 240 MHz, 360 x 360 QSPI panel, `slate-debug` on, 60 s):

| | First version | Regions and underlay (16 KB instruction cache) | Now (32 KB instruction cache) |
| --- | --- | --- | --- |
| Frames per second | 14 | 24 | 55 |
| Composing a frame (median) | 58 ms | 27 ms | 13 ms |
| Dirty share of the screen, regions | 43 %, 2 | 18 %, 11 | 16 %, 10 |

The changed elements stay in their own small regions, and the radial gradient and the track under them are copied
from the underlay instead of computed again. What is left is the drawing of the moving elements themselves, and on
the ESP32-S3 much of it is spent fetching code and external data: the compositor and the PHP interpreter on the
other core share the instruction cache and the external RAM, so in a busy scene a small element costs several
times what it costs alone (measured: a band composed while the PHP listener runs takes 1.5 to 2 times as long). The
work that remains is about one refresh period per frame, so some frames still miss a refresh. The
gauge example (a dial, an arc moved by the IMU and an orbiting dot) runs at 60 frames per second.

## Animations and transitions

Tweens and transitions cost nothing in PHP: the compositor samples them at every refresh of the panel and composes
only what they changed. Measured on an ESP32-S3 at 240 MHz with the 360 x 360 QSPI panel and its TE pin, on the
pages of the `06-display/slate-transitions` example (each page keeps its own tweens running, `repeat: -1`, also
during the transitions), page changes driven by a timer, four runs of six to twelve transitions of each kind; with
and without `slate-debug`, the same within 1 frame per second:

| Transition | Frames | Frames per second | Frame, average / longest | Longest frame with `keepScenes = 0` |
| --- | --- | --- | --- | --- |
| `push`, 300 ms | 16 to 19 | 57 to 58 (53 to 60) | 15.1 / 21 ms | 25 ms |
| `cover`, 350 ms (a panel over the page) | 20 to 22 | 59 to 60 | 13.7 / 28 ms (the panel's first time) | 28 ms, every time |
| `uncover`, 300 ms | 17 to 18 | 59 to 60 | 14.4 / 17 ms | 25 ms |
| `fade`, 400 ms | 8 to 9 | 19 to 22 | 42 to 49 / 70 ms | 84 ms |

- During a slide (`push`, `cover`, `uncover`) the whole screen is composed at every frame, both scenes band by band:
  a frame costs about what the two pages cost together, here 13 to 17 ms, so the panel's 60 refreshes a second are
  nearly all met. The frame after the end composes the new page alone (13 to 20 ms).
- A `fade` composes the scene coming in apart and blends it over the other one, as a full-screen group with an
  `$opacity` below 1 is (about 50 ms, see [Full-screen times](#full-screen-times)): about 20 frames per second. It
  is still smooth enough for a short fade; for large, busy pages a slide costs less.
- **Kept caches.** With the default `Slate::$keepScenes = 2`, a page with a shadow that is shown again builds
  nothing: no cache was built in any of the 48 transitions after the first visit of each page. With
  `$keepScenes = 0` each return builds the shadow again, in the first frame of the transition: 4 to 8 ms more for
  that frame in a slide, 15 ms more in a fade. Keeping the caches of two pages took 84 KB of PSRAM here (a card and
  a panel with a shadow, about 42 KB each).
- **Many tweens.** Circles moving with `animate(['x' => ..., 'y' => ...], repeat: -1, yoyo: true)`, each with
  its own time, over an opaque background: 20 of them run at 60 frames per second, composing 3.7 ms per frame (9 %
  of the screen dirty); 50 of them also at 60, composing 8 ms (20 % dirty).
- **Memory.** Without Wi-Fi, through all of the above: at least 80 KB of internal RAM free at every frame, PSRAM
  free never under 630 KB of the 1.3 MB left outside PHP. The compositor's task used at most 6.3 KB of its stack (a shadow built during a fade); the stack is 10 KB, which leaves room for deeper
  nesting of cached and blurred groups inside a fade.
- **Waiting between frames.** The compositor's task sleeps until the next TE pulse or deadline; with the 1 kHz
  tick of the esp32 SDK even a wait of a millisecond blocks. Measured: one wait in 150 s returned without blocking
  (0.2 ms in all).

## Scroll

A `Scroll` costs nothing in PHP either: Slate routes the touches to it, and the compositor runs the drag, the
inertia, the snapping and the springs at every refresh of the panel, composing only the view. Measured on an
ESP32-S3 at 240 MHz with the 360 x 360 QSPI panel and its TE pin, on the pages of the `06-display/slate-scroll`
example (views of 240 x 240 and 240 x 180), with touches sent by a timer every 16 ms, two rounds of each case:

| Case | Frames per second | Frame, average / longest | Compose, average / longest |
| --- | --- | --- | --- |
| A list of 40 rows (48 px, a card, a dot, two texts), no cache: drag | 60 | 13.9 / 14.6 ms | 7.6 / 11.1 ms |
| The same list: inertia after a fling | 55 (see below) | 13.5 / 13.9 ms | 7.0 / 9.4 ms |
| The same list, snapping on the rows (`snapY: 48`) | 57 to 60 | 13.6 / 13.9 ms | 5.7 / 6.9 ms |
| The same list, spring back from past an edge | 53 to 54 (see below) | 13.5 / 13.7 ms | 6.9 / 8.0 ms |
| The same list, `scrollTo()` 300 and 600 ms | 60 | 13.5 / 13.7 ms | 5.7 / 6.4 ms |
| A list of 12 rows with `$cache` (240 x 576, in the cache): drag | 58 | 15.8 / 18.7 ms | 13.5 / 16.0 ms |
| The same list with `$cache`: inertia | 60 | 15.9 / 17.6 ms | 12.8 / 13.9 ms |
| The same list with `$cache`: spring back from past an edge | 40 (see below) | 16.0 / 16.7 ms | 12.6 / 13.1 ms |
| The same list without `$cache`: inertia | 60 | 13.5 / 13.9 ms | 6.9 / 7.8 ms |
| The same list without `$cache`: spring back from past an edge | 38 (see below) | 13.6 / 13.8 ms | 6.9 / 7.9 ms |
| A carousel of six cards (`snapX: 240`): drag, snap | 58 to 60 | 11.6 / 12.3 ms | 5.0 / 9.2 ms |
| A 720 x 720 grid on both axes: drag | 60 | 14.4 / 17.4 ms | 10.1 / 13.8 ms |
| The same grid: inertia | 54 (see below) | 13.6 / 15.3 ms | 9.1 / 11.7 ms |
| The same grid: spring back from a corner | 53 (see below) | 13.6 / 14.3 ms | 8.2 / 9.5 ms |
| `push` between the pages | 60 | 14.4 / 15.3 ms | 10.4 / 11.8 ms |

- **Every frame is shorter than a refresh.** The longest frame of a moving list, uncached, is 14.6 ms, under the
  16.7 ms of the panel's refresh. The rates under 60 during inertia and springs come from their slow end: when the
  content moves less than a pixel between two refreshes nothing changes on the screen, and no frame is composed. The springs of the 12-row list are the lowest, 37 to 40 frames per second: its overshoot is
  short, so the slow tail is a large share of it, and the frames it does draw are as short as in the other cases. If
  your board shows about 38 fps there, it is this effect, not a slow frame.
- **`$cache` on a list.** The 40-row list is 1.38 MB as one image, over the caches' budget: it is composed directly,
  as without `$cache`, at the same rates. The 12-row list fits, but copying its image costs more than composing its
  few simple rows (compose 12.8 ms against 6.9 ms), and building it takes one frame of about 50 ms when the page is
  first shown. Keep `$cache` for a small content with costly children (shadows, blurs, gradients).
- **Snapping and springs** cost what a frame of the same content costs: the curve and the spring are a few
  operations per axis per frame.
- **Memory.** Through all of the above, without Wi-Fi: at least 77 KB of internal RAM free at every frame, PSRAM
  free never under 560 KB (with the cache of the 12-row list), and the compositor's task used at most 4.1 KB of its
  10 KB stack.

## Touch

A scene without touchable elements costs nothing more for touch: no table, no work per frame. With touchable
elements the compositor writes a table of where they are after each frame that changed something, on its own task,
once the frame has gone to the panel, and PHP looks the finger up in it at each `Pressed`. Measured on an ESP32-S3
at 240 MHz with the 360 x 360 QSPI panel, with touch events built by a timer and delivered through the same path as
the touch screen's, two rounds of each case:

| Scene | Entries in the table | Building the table, average / longest | Frames |
| --- | --- | --- | --- |
| 50 touchable rectangles, a spinner that changes every frame | 50 | 0.43 / 0.58 ms, after every frame | 60 per second, 13.6 ms on average: as with none touchable (60, 13.5 ms) |
| The same, nothing moving | 50 | none: no frame, no table | none |
| The busy scene above, its 18 moving elements touchable | 18 | 0.34 / 1.8 ms, after every frame | 55.8 to 55.9 per second, against 56.2 to 56.5 with none touchable; compose 12.6 to 12.7 ms in both |
| A list of 40 touchable rows (a group of four elements each) in a `Scroll`, while it scrolls | 8 to 9 | 0.75 / 1.9 ms, after every frame | drag 60, inertia 55 per second: as before the rows were touchable |
| A carousel of eight touchable cards (`06-display/slate-touch`) while it moves | 4 to 5 | 0.32 to 0.36 / 0.76 ms | |

- **It does not lengthen a frame.** The table is built after the frame, in the time the compositor would otherwise
  wait for the next refresh. Only a scene whose frames already fill the refresh loses frames to it: the busy scene,
  about half a frame per second. Scrolling scenes that are bound by the refresh can lose up to about one frame per
  second: a list springing back 53.6 against 54.7, a 2D area at its corner 52.4 against 53.3, a 2D drag 59.5 against
  60.1. These differences are within the spread between measuring sessions, so they are not proof of a cost, but do
  not expect them to be zero.
- **It grows with the touchable elements walked, not with the ones you see.** The 40 rows of the list are all
  walked at every frame of a scroll, although five are in the view, and the box of a touchable group is the box of
  its children. Make a row or a card touchable as a whole (its children are part of its area) rather than each of
  its parts, and keep the touchable elements of a long list few.
- **From the touch to the handler.** From the `$time` of the touch event to the first line of `onPress()` or of
  the `Tap` callback, through Slate's routing on the PHP task: 0.4 to 0.5 ms (median) on a still screen, 0.7 ms
  while the compositor draws every refresh, 1.7 ms at most. The time the touch controller takes to report the
  finger comes before it.
- **Memory**, in external RAM: two tables of 20 bytes per touchable element (the one read and the one being
  written) and an index of the touchable elements; about 3.6 KB for 50. The tables keep their largest size while
  the Slate runs: a scene with many touchable elements leaves that memory in use after it is left, until the
  Slate stops.

## Measuring

Build with the `slate-debug` option: the frames per second appear on screen and the console prints the time
of a frame, the dirty share of the screen and the cache use every second (see [Debug](./debug.md)).
