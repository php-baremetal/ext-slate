---
eyebrow: 'Package · Extension'
lede:    'A retained-mode 2D scene graph for display panels: describe a scene of shapes, text, images and pixels once, change properties, and Slate redraws only what changed, band by band, at the pace of the panel.'
see_also:
  - { href: './usage/scenes.md', meta: 'API' }
  - { href: './usage/elements.md', meta: 'API' }
  - { href: 'package:php-baremetal/ext-display', meta: 'Extension', label: 'ext-display' }
  - { href: 'package:php-baremetal/ext-events', meta: 'Extension', label: 'ext-events' }
  - { href: 'package:php-baremetal/ext-input', meta: 'Extension', label: 'ext-input' }
  - { href: 'package:php-baremetal/esp32-display-st77916', meta: 'Driver', label: 'esp32-display-st77916' }
prev: { label: 'No previous page', href: '#' }
next: { label: 'Scenes and pages', href: './usage/scenes.md' }
---

# Slate

Slate draws user interfaces on a display panel. A script describes **scenes** made of **elements** (rounded
rectangles, circles, arcs, lines, text, images, groups and canvases with pixels of their own) and then only changes
their properties: `$label->text = '92 °C'`, `$bar->to = 180`. A compositor task of its own works out which part
of the screen changed and composes only that, with anti-aliased edges, alpha blending and, when they are built,
gradients, soft shadows and blur, and sends it to the panel in bands of rows. A screen that does not change
costs nothing: no frame is composed.

- `Baremetal\Slate\Slate`: takes the panel of a display and shows one scene at a time.
- `Baremetal\Slate\Scene`: a screen of the application; a page is a class that extends it.
- `Element` and its classes `RoundRect`, `Circle`, `Arc`, `Line`, `Text`, `Group`, `Canvas`, `Image`.
- `Images::preload()` and `Preload`, the events `ImageLoaded`, `ImageFailed`, `PreloadProgress`, `PreloadDone`.
- `Element::animate()`, `Element::ease()`, `Tween` and the event `TweenDone`: animations run by the compositor.
- `Transition` and the event `TransitionDone`: page transitions of `Slate::show()` (fade, push, cover, uncover).
- `Scroll` (a `Group` with a view the finger drags, with inertia, snapping and bars) and the event `ScrollEnded`.
- `Caps`, `Align`, `ImageError`, `Ease`, `Direction` and `ScrollAxis` (enums), `LinearGradient`, `RadialGradient`, `Shadow`.

## Requirements

ext-slate is platform-neutral C: it has no HAL of its own.

| Requires | For |
| --- | --- |
| `php-baremetal/ext-display` `^1.1` | The display object and the band path of its panel driver. The driver must implement the band path: on the esp32 SDK, [esp32-display-st77916](package:php-baremetal/esp32-display-st77916) 1.2 or later does. |
| `php-baremetal/ext-events` `^1.2` | The task of the compositor, its queues and memory (the events HAL, `php-baremetal/events-hal` 1.5, provided on esp32 by `esp32-events-hal`, which conductor adds for the project's platform), and the moment PHP returns to the event loop, when the changes are handed over. |
| `php-baremetal/ext-input` `^1.0` | The touch events, routed to the elements and the scene. |
| `php-baremetal/bm-common` `^1.1` | The board's blocks, for `Slate::onboard()`. |

On the board the compositor task runs on core 0, while PHP runs on core 1; elements, texts, element caches and
images live in external RAM (PSRAM), the bands of the frame in internal RAM from the panel driver. Images are read
with the C standard I/O by a loader task of their own, from the paths where phpbm puts the project's assets
(`/app/...` in flash, `/sdcard/...` on the microSD card, see [Images](./usage/images.md)).

## Install

```bash
composer require php-baremetal/ext-slate
```

A project for the Waveshare ESP32-S3-Touch-LCD-1.85B (display ST77916, touch CST816S), with the default font:

<!-- @code-block language="json" label="composer.json" -->
```json
{
  "name": "acme/slate-hello",
  "type": "project",
  "repositories": [
    { "type": "composer", "url": "https://repository.php-baremetal.com", "canonical": true }
  ],
  "require": {
    "php-baremetal/conductor": "^1.0",
    "php-baremetal/ext-spi": "^1.0",
    "php-baremetal/esp32-display-st77916": "^1.2",
    "php-baremetal/touch-cst816s": "^1.0",
    "php-baremetal/ext-slate": "^1.0"
  },
  "config": {
    "allow-plugins": { "php-baremetal/conductor": true },
    "platform": { "php": "8.5.11" }
  },
  "extra": {
    "baremetal": {
      "sdk": "esp32@^2.0",
      "board": "waveshare-esp32-s3-touch-lcd-1.85b",
      "runtime": "event-driven",
      "storage": "embedded",
      "fonts": { "default": true }
    }
  }
}
```
<!-- @endcode-block -->

## Example

<!-- @code-block language="php" label="index.php" -->
```php
<?php
use Baremetal\Events;
use Baremetal\TimerEvent;
use Baremetal\Slate\{Slate, Scene, RoundRect, Arc, Text, Align};

final class Tick extends TimerEvent {}

final class Home extends Scene
{
    public Arc $bar;
    public Text $value;

    protected function build(): void
    {
        $this->add(
            new RoundRect(60, 60, 240, 240, radius: 120, fill: 0x202830),
            $this->bar = new Arc(180, 180, 100, from: -90, to: -90, width: 12, color: 0xE0A040),
            $this->value = new Text(180, 170, '0 %', align: Align::Center),
        );
    }
}

$slate = Slate::onboard();   // the board's display and touch screen
$home = new Home();
$slate->show($home);

$v = 0;
Events::listen(Tick::class, function () use ($home, &$v) {
    $v = ($v + 1) % 101;
    $home->bar->to = -90 + 3.6 * $v;      // both changes go in the same frame
    $home->value->text = "$v %";
});
every(50, Tick::class);
```
<!-- @endcode-block -->

## When the changes reach the screen

Writing a property does not draw: the change is queued for the compositor and handed over when PHP returns to
the event loop. In the `event-driven` runtime that is after each listener: every change one listener makes
appears in the same frame, never half of them. In `init-loop` it is after each `loop()`. `Slate::show()` hands
the changes over at once, and `Slate::commit()` does so for a script that never returns to a loop. The
compositor then composes at the next TE pulse of the panel (or at its refresh rate when the panel has no TE
pin).

Touch events reach `Scene::onTouch()` in the `event-driven` runtime, where ext-events delivers events.

## Notes and limits

- One Slate at a time, on any panel: a second one throws `\LogicException` while the first one lives. While a
  Slate drives a display, the display's drawing methods and `flush()` throw `\LogicException` (`<Class>: the
  panel is driven by Slate; draw in a Slate Canvas`); `width()`, `height()`, `backlight()`, `sleep()` and
  `wake()` keep working. While the panel sleeps (`sleep()`) Slate starts no new frame (one already under way finishes) and the changes wait; after
  `wake()` they go, and a driver that re-initialises the panel on wake (St77916 does) gets the whole screen again.
  Destroying the Slate (`unset()`, or the end of the program) gives the panel back.
- A panel driver without the band path: `\LogicException` `Slate: <Class> has no band path`.
- Wrong values throw `\ValueError` naming the property and its limit (`Baremetal\Slate\RoundRect::$opacity must
  be between 0 and 1`); the old value stays.
- If the panel stops answering (a band not finished within 2000 ms), the compositor stops sending, and the next
  call that changes the scene throws `\RuntimeException` `Slate: the panel did not finish a band within 2000 ms`.
  A driver that marked the panel failed, or that refuses every band for 4 frames in a row, gives
  `Slate: the panel refused the bands (error 0x..)` the same way.
- The compositor never calls PHP: a component of your own is a `Group` of the built-in elements.
- The screen's background is black: for another colour, put a full-screen `RoundRect` (or an `Image`) at the bottom
  of the scene.
- Images are loaded in the background; until an image is there its element draws nothing. A bad or missing file
  raises `ImageFailed` and never breaks the screen.
- Animations run in the compositor, sampled at the panel's refresh: `$card->animate(['x' => 200], ms: 300)`
  returns a `Tween`, and `TweenDone` comes when it ends. A screen with no animation running composes no frame.
  `$slate->show($menu, Transition::push(Direction::Left))` slides to another page the same way.
- A `Scroll` takes the touches by itself, and a drag that starts on one of its touchable rows passes to it past
  8 px; the drag, the inertia, the spring past an edge and the snapping run in the compositor, and `ScrollEnded`
  comes when the content rests.

See [Scenes and pages](./usage/scenes.md), [Elements](./usage/elements.md), [Images](./usage/images.md),
[Animations](./usage/animations.md), [Scroll](./usage/scroll.md),
[Effects and build options](./usage/effects-and-options.md), [Performance](./usage/performance.md) and
[Debug](./usage/debug.md).
