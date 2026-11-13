---
eyebrow: 'ext-slate · PHP API'
lede:    'Image shows a picture from a file, loaded in the background; Images::preload() loads the next page ahead of time. The project''s PNG assets become .sli files at build time, the fast path.'
see_also:
  - { href: './elements.md', meta: 'API' }
  - { href: './performance.md', meta: 'API' }
  - { href: './effects-and-options.md', meta: 'API' }
  - { href: 'package:php-baremetal/ext-events', meta: 'Extension', label: 'ext-events' }
prev: { label: 'Elements', href: './elements.md' }
next: { label: 'Animations', href: './animations.md' }
---

# Images

An `Image` element draws a picture from a file on the board: a `.sli` file, Slate's own run-length format, or a
`.png` decoded on the board. The file is loaded in the background as soon as the element is made; until it is
there the element draws nothing, and an event says when it is ready. Images of the same file are loaded once and
shared.

<!-- @code-block language="php" label="Images" -->
```php
<?php
use Baremetal\Events;
use Baremetal\Slate\{Image, Images, ImageLoaded, ImageFailed};

$bg   = new Image(0, 0, '/app/assets/ui/bg.sli');                 // loading starts now
$logo = new Image(20, 20, '/sdcard/assets/photos/logo.sli', opacity: 0.8);
$scene->add($bg, $logo);

Events::listen(ImageFailed::class, function (ImageFailed $e) {
    echo "{$e->path}: {$e->message}\n";
});

$next = Images::preload([                       // the next page, ahead of time
    '/app/assets/ui/menu-bg.sli',
    '/sdcard/assets/photos/beans.sli',
]);
```
<!-- @endcode-block -->

## Where the files come from

Images are assets of the project, declared in `extra.baremetal.assets` of `composer.json`: a list of folders of
the project, each with the storage it goes to.

<!-- @code-block language="json" label="composer.json (excerpt)" -->
```json
{
  "extra": {
    "baremetal": {
      "assets": [
        { "dir": "assets/ui",     "storage": "embedded" },
        { "dir": "assets/photos", "storage": "microsd" }
      ]
    }
  }
}
```
<!-- @endcode-block -->

At every `build`, `flash` and `sync`, phpbm (0.3 or later) converts the `.png` files of those folders (subfolders included) into
`.sli` files, copies the `.sli` files it finds as they are, and puts them on the board: the `embedded` folders in
the read-only storage of the firmware's flash, next to the sources, and the `microsd` folders on the microSD card
(only the files that changed are copied). A conversion is redone only for the files that changed.

The path on the board is fixed: the storage's mount point, then the folder's path in the project, with `.png`
changed to `.sli`:

| In the project | Storage | On the board |
| --- | --- | --- |
| `assets/ui/icons/coffee.png` | `embedded` | `/app/assets/ui/icons/coffee.sli` |
| `assets/photos/beans.png` | `microsd` | `/sdcard/assets/photos/beans.sli` |

That path is what the PHP code writes: there are no logical names or search roots. For a file you put on the board
yourself, `phpbm image:convert <in.png> <out.sli>` converts it.

A `.sli` file is the fast path: it is read into memory as it is, with no decoding, and drawn row by row, run by
run. A `.png` path (with `png` in the `slate-images` option, the default) is decoded on the board, which is slower
and takes more memory for a while (see [Memory](#memory)); keep it for files that reach the board some other way.

## Image

<!-- @method name="Image::__construct(int|float $x, int|float $y, string $src, float $opacity = 1.0)" -->

An element like the others: its top-left corner at (`$x`, `$y`) of the parent, rounded to whole pixels, with
`$visible` and `$opacity`. Its size is the file's.

| Property | Type | Meaning |
| --- | --- | --- |
| `$src` | `string` | The file: an absolute path ending in `.sli` or `.png` (in any case). A new path loads that image; the element is empty until it is there. Writing the path of an image that failed loads it again. |
| `$w`, `$h` | `int`, read-only | The image's size in pixels; 0 until it is loaded, or when it failed. |
| `$loaded` | `bool`, read-only | `true` once the image is loaded. |

`$w`, `$h` and `$loaded` change when the `ImageLoaded` event is delivered (after the return to the event loop, as
every event), or in `wait()`. An `Image` of a path that is already in memory (shown by another `Image`, or
preloaded) is ready at once: `$loaded` is `true` right after `new`, and no event is raised for it.

<!-- @method name="Image::wait(int $timeout_ms = 5000): bool" returns="bool" -->

Blocks until the image is loaded, at most `$timeout_ms` milliseconds, and updates `$w`, `$h` and `$loaded`.
`true` when the image is loaded; `false` on timeout or when it failed. No other event is delivered meanwhile, and
the `ImageLoaded` event still comes after. Use it in a script that has no event loop, or where a page must not
appear before its background.

The screen is never broken by a bad file: an image that cannot be loaded leaves the element empty and raises
`ImageFailed`.

## Preloading

<!-- @method name="Images::preload(array $paths): Preload" returns="Preload" -->

Starts loading every path and returns a `Preload` that keeps the images in memory while it lives: an `Image` of
one of those paths is then ready at once. Typically the images of the next page, preloaded while the current one
is shown.

| Preload | Meaning |
| --- | --- |
| `$loaded` (`int`, read-only) | Images loaded so far. |
| `$total` (`int`, read-only) | The number of paths. |
| `$done` (`bool`, read-only) | Every image is loaded or failed. |
| `wait(int $timeout_ms = 5000): bool` | Blocks until no image is still loading, at most `$timeout_ms`: `true` when every image is loaded. |

The images already in memory are counted at once. `PreloadProgress` follows each image loaded after the call,
`PreloadDone` the last one finished, also when every image was in memory already (it then comes after the return
to the event loop). Every `Preload` gets exactly one `PreloadDone`, also when a burst of images overflows the
event pool of ext-events. An image is in memory while an `Image` shows it or a `Preload` holds it, and freed after.

<!-- @code-block language="php" label="Next page" -->
```php
$menuImages = Images::preload(['/app/assets/ui/menu-bg.sli', '/app/assets/ui/icons/coffee.sli']);
// PreloadDone comes for every Preload: check that it is this one that finished
Events::listen(PreloadDone::class, function (PreloadDone $e) use ($slate, $menu, $menuImages) {
    if ($menuImages->done && $slate->scene() !== $menu) {
        $slate->show($menu);   // its images are in memory: drawn at the first frame
    }
});
```
<!-- @endcode-block -->

## Events

Events of ext-events (final classes that extend `Baremetal\Event`), delivered like the others, after the return
to the event loop (in the `event-driven` runtime; `init-loop` delivers no events, but the properties of `Image`
and `Preload` still follow at the end of each `loop()`):

| Event | Properties | When |
| --- | --- | --- |
| `ImageLoaded` | `string $path`, `int $w`, `int $h` | An image finished loading: once per file, however many elements show it. |
| `ImageFailed` | `string $path`, `ImageError $error`, `string $message` | An image could not be loaded. |
| `PreloadProgress` | `int $loaded`, `int $total` | One more image of a `Preload` is loaded. |
| `PreloadDone` | `array $failed` (a list of paths) | Every image of a `Preload` is loaded or failed; `$failed` lists the paths that failed. |

## Errors

Wrong paths throw at once, when the element is made or `$src` is written (the old `$src` stays):

| Thrown | Message |
| --- | --- |
| `\ValueError` | `Baremetal\Slate\Image::$src must be an absolute path` |
| `\ValueError` | `Baremetal\Slate\Image::$src must end in .sli or .png` |
| `\ValueError` | `png is not built in: add it to slate-images in composer.json` (a `.png` path without `png` in `slate-images`) |

`Images::preload()` checks every path before loading any, with messages of its own:

| Thrown | Message |
| --- | --- |
| `\ValueError` | `Baremetal\Slate\Images::preload(): Argument #1 ($paths) must hold absolute paths, "<path>" is not one` |
| `\ValueError` | `Baremetal\Slate\Images::preload(): Argument #1 ($paths) must hold paths that end in .sli or .png, "<path>" does not` |
| `\ValueError` | `png is not built in: add it to slate-images in composer.json` |
| `\TypeError` | `Baremetal\Slate\Images::preload(): Argument #1 ($paths) must contain only strings, <type> given` |

Three more errors are thrown at once:

| Thrown | When |
| --- | --- |
| `\RuntimeException` | `Slate: no memory for the image <path>`: not even the image's small record fits in memory (`new Image`, `$src`, `Images::preload()`). |
| `\Exception` | `<function>() is not available in the web-server runtime: event sources need the event-driven runtime` (from ext-events): the first image of a request in the `web-server` runtime, since images raise events. |
| `\RuntimeException` | `Slate: cannot load images: the table of ext-events' idle hooks is full`: other extensions took the four hooks ext-events keeps. |

Whether a file really is what its name says is known only when it is read: the errors of loading come as
`ImageFailed`, with `$error` of the enum `ImageError: string`:

| Case | Value | Meaning |
| --- | --- | --- |
| `NotFound` | `'not-found'` | No such file (a microSD card not mounted, a path mistyped). |
| `Unreadable` | `'unreadable'` | The file could not be read. |
| `BadFormat` | `'bad-format'` | Not a valid `.sli` or PNG: a damaged or truncated file, or another format under that name. |
| `Unsupported` | `'unsupported'` | A valid PNG that is not decoded on the board: 16 bits per channel or interlaced. phpbm converts those: make it an asset. |
| `NoMemory` | `'no-memory'` | Not enough memory for the image: the external RAM left outside PHP is used up (on the esp32 SDK, raise its `firmware-reserve` option, see [Memory](#memory)). |

A failed image is tried again by the next `Image` (or `$src`) of its path: a file written later, or a card
mounted later, then loads.

## Memory

- Images live in external RAM (PSRAM), outside the budget of the element caches: a `.sli` file takes its own size
  (about 2 bytes per opaque pixel, less for runs of one color and for transparent parts, up to 3 bytes for
  semi-transparent pixels). On a board without PSRAM they take internal RAM.
- That external RAM is not PHP's `memory_limit`: it is the part the SDK leaves to the rest of the firmware, shared
  with the element caches (whose budget is half of the largest free block when the Slate starts), the network and
  other native extensions. On the esp32 SDK its size is the `firmware-reserve` option (in KB, default `1024`): PHP
  takes the rest of the PSRAM. With the default, about 1066 KB were free after the start on an ESP32-S3 with 8 MB
  of PSRAM, of which the caches may take up to half: enough for a few photos of 360 x 360 (about 250 KB each), not
  for a full-screen background on a larger panel.
- To make room, raise `firmware-reserve`: `memory_limit` shrinks by as much (in steps of 256 KB; the reserve is at
  most 4096 KB and at most a quarter of the free PSRAM). The caches' budget grows with it (half of the largest free
  block), so in the worst case an image needs about twice its size of extra reserve. Keep only the images of the current
  page and the next in memory (an image is freed when no `Image` shows it and no `Preload` holds it), and convert
  PNG files with phpbm instead of decoding them on the board. For example, an opaque 1024 x 600 background is a
  `.sli` of about 1.2 MB: on a board with 32 MB of PSRAM, a reserve of 3072 KB leaves it room next to the caches
  (see the excerpt after this list).
- When an image cannot be loaded for lack of memory (reading a `.sli`, decoding a PNG), the caches the scenes left
  keep (see [Returning to a scene](./scenes.md#returning-to-a-scene)) are dropped and the image is loaded once
  more; only then does it fail with `no-memory`. The loader asks the compositor for it and waits at most 100 ms.
- Images are loaded one at a time, in the order they were asked for, by a task of low priority that exists only
  while there is something to load (6 KB of stack); PHP and the compositor go on meanwhile.
- A PNG is decoded row by row straight into a `.sli`: never the whole image as pixels, but at the end of the
  decoding the result is held twice for a moment, so the peak is about twice the `.sli` size, plus about 37 KB
  while it decodes. An 800 x 480 photo is about 770 KB of `.sli` and about 1.5 MB of PSRAM at the peak: more than
  the default `firmware-reserve` leaves, so it fails with `no-memory` unless the reserve is raised to about
  `3328` in the worst case (on a board with enough PSRAM). Converted by phpbm, the same image costs only its `.sli` size, and loads
  faster.
- PNG decoded on the board: grey, grey with alpha, RGB, RGBA and palettes (with or without `tRNS`) at 8 bits, grey
  and palettes also at 1, 2 and 4 bits; at most 32767 x 32767 pixels, and what fits in memory. Colors are reduced
  to the panel's 16 bits with the same ordered dither as gradients: the result is the same `.sli` phpbm writes,
  byte for byte.

<!-- @code-block language="json" label="composer.json (excerpt)" -->
```json
"extra": {
  "baremetal": {
    "options": { "firmware-reserve": 3072 }
  }
}
```
<!-- @endcode-block -->
