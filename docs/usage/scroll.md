---
eyebrow: 'ext-slate · PHP API'
lede:    'Scroll is a group that shows part of its content through a view: the finger drags it, a release flings it on with inertia, past an edge it stretches and springs back, it can snap to rows or pages, and scrollTo() moves it in an animation; the compositor runs it all, and ScrollEnded says when it comes to rest.'
see_also:
  - { href: './elements.md', meta: 'API' }
  - { href: './animations.md', meta: 'API' }
  - { href: './scenes.md', meta: 'API' }
  - { href: './performance.md', meta: 'API' }
  - { href: 'package:php-baremetal/ext-input', meta: 'Extension', label: 'ext-input' }
prev: { label: 'Animations', href: './animations.md' }
next: { label: 'Effects and build options', href: './effects-and-options.md' }
---

# Scroll

A `Scroll` is a `Group` with a view: the box (0, 0, `$w`, `$h`), always clipped. Its children are the content, placed
in content coordinates from (0, 0); the compositor draws them shifted by the scroll position. The content moves under
the finger, goes on with inertia when it is released, stretches past an edge and springs back, and can come to rest
on a step (a row, a page). All of it runs in the compositor at the pace of the panel, like the animations: PHP
forwards the touches and is told when the content comes to rest.

<!-- @code-block language="php" label="A list" -->
```php
<?php
use Baremetal\Input\{Released, TouchEvent};
use Baremetal\Slate\{RoundRect, Scene, Scroll, ScrollAxis, Text};

final class Contacts extends Scene
{
    private Scroll $list;

    protected function build(): void
    {
        $this->list = new Scroll(x: 20, y: 60, w: 320, h: 240, axis: ScrollAxis::Vertical, snapY: 48);
        foreach (['Ada', 'Alan', 'Grace', 'Linus', 'Margaret'] as $i => $name) {
            $this->list->add(
                new RoundRect(0, $i * 48, 320, 46, radius: 8, fill: 0x202830),
                new Text(16, $i * 48 + 14, $name),
            );
        }
        $this->add($this->list);
    }

    public function onTouch(TouchEvent $e): void
    {
        if ($this->list->touch($e)) {
            return;   // a drag: not a tap
        }
        if ($e instanceof Released) {
            $this->tapAt($e->x, $e->y);
        }
    }

    private function tapAt(int $x, int $y): void
    {
        $row = intdiv($y - 60 + (int) round($this->list->scrollY), 48);   // the row under the finger
        // ...
    }
}
```
<!-- @endcode-block -->

## The Scroll

<!-- @method name="Scroll::__construct(int|float $x = 0, int|float $y = 0, int|float $w = 1, int|float $h = 1, ScrollAxis $axis = ScrollAxis::Both, bool $bounce = true, int $snapX = 0, int $snapY = 0, int $snapMax = 0, bool $indicators = true, int $contentW = 0, int $contentH = 0, float $opacity = 1.0, bool $cache = false)" -->

`add()` and `remove()` work as for any group; the children are the content. The properties are read and written
like the others; a value out of range throws `\ValueError` and the old value stays.

| Property | Meaning |
| --- | --- |
| `$w`, `$h` | The view, 1 to 32767. |
| `$contentW`, `$contentH` | The size of the content, 0 to 32767. 0 (the default): the box of the children from (0, 0), computed by the compositor and kept up to date when the children change. |
| `$axis` | `ScrollAxis::Both` (default), `ScrollAxis::Vertical` or `ScrollAxis::Horizontal`: the axes the content moves on. |
| `$bounce` | `true` (default): past an edge the content stretches with resistance and springs back. `false`: it stops at the edge. |
| `$snapX`, `$snapY` | The snap step in pixels, 0 to 32767; 0: none (see [Snapping](#snapping)). |
| `$snapMax` | At most this many snap steps per release, counted in the direction of the fling from where the finger lifts, 0 to 32767; 0 (the default): no limit. `1` makes a carousel move one card per fling (see [Snapping](#snapping)). |
| `$indicators` | Bars that show the position while the content moves (see [Indicators](#indicators)). |
| `$scrollX`, `$scrollY` | The position. Read: the one last drawn. Written: the content jumps there, kept within the limits (no error), and any motion stops. |
| `$clip` | Always `true`; writing `false` throws `\ValueError`. |
| `$cache` | Keeps the **whole content** as one image (see [Cache and performance](#cache-and-performance)). |

The common properties (`$x`, `$y`, `$visible`, `$opacity`) and `$blur`, `$shadow` are those of `Group` and apply to
the view.

- **Limits.** On each axis the position goes from 0 to `max(0, content - view)`. A content smaller than the view
  does not scroll on that axis (with `$bounce` it still stretches under the finger). An axis that `$axis` does not
  allow never moves.
- **The position read.** `$scrollY` is the position the compositor last drew, in whole pixels; during the stretch
  past an edge it is below 0 or above the limit. Right after a write (and before the scroll is first drawn) it reads
  the value written, until the compositor has drawn it: `$list->scrollY = 5000` reads `5000`, and the limit once it
  is drawn.
- **Kept pages.** A scene left and shown again keeps the position: the content is where it was. While its scene is
  not shown, a motion pauses, and goes on when it is shown again; during a page transition it goes on.
- **Animations.** `animate()` and `ease()` of `$x`, `$y`, `$opacity`, `$w`, `$h`, `$blur` work as for a group. The
  position, the content size, the snap steps and `$snapMax` are not animated that way: use `scrollTo()`.

A class of your own can extend `Scroll` (a list with its rows, for example).

## touch() and taps

<!-- @method name="Scroll::touch(\Baremetal\Input\TouchEvent $e): bool" returns="bool" -->

Slate does not decide which element a touch is for: the script forwards the touch events to the scroll, usually from
`Scene::onTouch()`. `touch()` returns `true` when the touch belongs to the scroll (a drag, or a catch of a moving
content), so the script knows that it is not a tap:

| Event | Returns |
| --- | --- |
| `Pressed` outside the view | `false`; the touch is ignored until the next `Pressed`. |
| `Pressed` inside the view | `false` (it may be a tap). If the content is moving (inertia, spring, snap, `scrollTo()`) it stops there and `touch()` returns `true`: the touch is a **catch**, the scroll's for its whole life. |
| `Moved` after a `Pressed` inside | `false` until the finger is more than **8 px** from the press, on the axis allowed (with `Both`, the distance); from there the drag starts and every `Moved` returns `true`. The content follows the finger from that point, without a jump. In a catch every `Moved` returns `true`, and a drag starts past 8 px as usual. |
| `Released` | After a drag: `true`, and the content goes on with the speed of the finger (the last 80 ms). After a catch: `true` (stopping the content is not a tap). Otherwise: `false` (a tap). |
| `Gesture` | `false`, ignored. |
| Any, on a scroll that is not in the scene shown (or not visible, or during a page transition) | `false`, nothing happens. |

The view is found on the screen from the PHP properties: the `$x`, `$y` of the scroll and of the groups it is in,
minus the position of the scrolls it is in, cut by every clipping ancestor (the view of each scroll it is in, the box
of each group with `$clip`): a press on a part of the view that is not on the screen is outside. A scroll inside
another scroll works; with the same touch for both, the script decides which one gets it. The properties are the PHP
values: while the scroll or an ancestor animates, the touch is tested against where it is going, not where it is
drawn.

Once a touch is the scroll's (a drag started, or its `Pressed` caught a moving content), it stays so to its end: its
`Moved` and `Released` return `true` even when the content stops following it. That happens when the script writes
`$scrollX`/`$scrollY`, calls `scrollTo()` or changes `$axis`, `$bounce`, `$snapX`, `$snapY`, `$snapMax` during the
touch (the
compositor ends the drag where it is: no inertia, no `ScrollEnded`), and when the scroll leaves the screen meanwhile
(removed, hidden, its page left: the compositor ends the drag as a release without speed). From then on nothing more
is sent for that touch, and no new drag starts before the next `Pressed`.

In `onTouch()` of the example above, a drag of the list (or a touch that stopped it) never reaches `tapAt()`; a
short touch on a list at rest does, and the
row under the finger is the touch's `y` relative to the view plus `$scrollY`.

## Snapping

With `$snapY = 48` the content comes to rest on a multiple of 48 (a row of a list); with `$snapX = 320` on a page of a
320 px carousel. At the release the compositor takes the point where the inertia would stop, rounds it to the nearest
step (the end of the content counts as a step, so the last page is reached) and glides there along `OutCubic`, in
150 to 600 ms depending on the speed. A release without speed snaps to the nearest step in 200 ms. Each axis snaps on
its own; `0` is no snapping.

A fast fling can travel several steps. `$snapMax` limits that, on each axis that has a snap step: the content rests
at most `$snapMax` steps beyond the step at or behind the point where the finger lifts, counted in the direction of
the fling (a fling forward counts from the step at or before that point, a fling back from the step at or after
it). The end of the content still counts as a step, and the limits still apply. The limit never sends the content
back against the fling: a slow drag across 3.4 cards followed by a hard fling forward rests on the
fourth card, not near where the touch began. A release without speed (under 20 px/s) rests on the nearest step, as
without a limit. With `snapMax: 1` a carousel moves at most one card past where it is let go, however fast. `0` (the
default) is no limit; on an axis without a snap step it does nothing.

<!-- @code-block language="php" label="A carousel: one card per fling" -->
```php
<?php
use Baremetal\Slate\{Scroll, ScrollAxis};

// six cards of 240 px; a fling, however hard, moves one card
$carousel = new Scroll(x: 60, y: 76, w: 240, h: 180, axis: ScrollAxis::Horizontal, snapX: 240, snapMax: 1,
                       indicators: false);
```
<!-- @endcode-block -->

## Indicators

With `$indicators` (the default) a bar 3 px thick, with rounded ends, runs 2 px inside the right edge of the view
(vertical position) and the bottom edge (horizontal position), over the content. Its length is
`view² / content` (at least 16 px), shorter while the content is stretched past an edge. The bars show while the
content moves, stay 500 ms after it stops, then fade in 200 ms. Only the axes that can scroll have one.

## scrollTo()

<!-- @method name="Scroll::scrollTo(int|float $x, int|float $y, int $ms = 300, Ease $ease = Ease::OutCubic): Tween" returns="Tween" -->

Moves the content to (`$x`, `$y`), kept within the limits, in `$ms` milliseconds along `$ease`. It returns a
[Tween](./animations.md#the-tween): `wait()`, `then()` and `stop()` work as for the others, and its `TweenDone`
arrives at the end, followed by the scroll's `ScrollEnded`.

- `$ms = 0`: a jump (as a write of `$scrollX`, `$scrollY`); the Tween is done at once and has its `TweenDone`, with
  no `ScrollEnded`. The same for a scroll that is not drawn yet.
- A press inside the view, a write of `$scrollX`/`$scrollY` or a change of `$axis`, `$bounce`, `$snapX`, `$snapY`,
  `$snapMax` stops it where it is: `$done` stays `false`, `wait()` returns `false`, no `TweenDone`.
- A new `scrollTo()` takes the position over from where it is; the earlier Tween still ends with its `TweenDone`.

<!-- @code-block language="php" label="Back to the top" -->
```php
$t = $this->list->scrollTo(0, 0, ms: 250);   // a tap on a "top" button
$t->then($this->title, ['opacity' => 1.0]);  // when the list is there
```
<!-- @endcode-block -->

## ScrollEnded

An event of ext-events (a final class that extends `Baremetal\Event`), delivered like `TweenDone`:

| Event | Properties | When |
| --- | --- | --- |
| `ScrollEnded` | `Scroll $scroll`, `int $x`, `int $y` | The content came to rest after a release (inertia, spring, snap) or at the end of a `scrollTo()`. Once per motion; never while the finger holds the content, nor after a jump or a press that stopped it. `$x`, `$y`: the position it rests at. |

<!-- @code-block language="php" label="Page dots of a carousel" -->
```php
<?php
use Baremetal\Events;
use Baremetal\Slate\ScrollEnded;

Events::listen(ScrollEnded::class, function (ScrollEnded $e) use ($carousel, $dots) {
    if ($e->scroll === $carousel) {
        $dots->select(intdiv($e->x, 320));
    }
});
```
<!-- @endcode-block -->

When a burst of events fills the event pool of ext-events, a `ScrollEnded` can be lost; the position read is
always right. A scroll freed (removed and no longer referenced) before its motion ends gets no `ScrollEnded`. In the
rare case it is freed after its end was queued, the event still arrives and carries a stand-in: a plain `Scroll`
that is drawn nowhere (`$e->scroll === $list` is false, and it is never an instance of a subclass).

## Cache and performance

Without `$cache`, each frame in which the position changes composes the view again, and only the children that are
in the view. A scroll that does not move costs nothing per frame, and a still screen gives no frame at all.

With `$cache` the whole content is kept as one image in external RAM, built once and copied at each position; it is
built again only when something in the content changes. It takes 2 bytes per pixel of the content (plus 1 for its
transparency), out of the caches' budget (see [Performance](./performance.md)). That budget is about half of the
largest free block of PSRAM when the Slate starts (at most 2 MB): about 496 KB on the 360 x 360 board measured, with
about 1 MB of PSRAM free in one block; it depends on the free PSRAM. A long full-width list usually does not fit (a
list of 40 rows of 320 x 48 is about 1.8 MB, so on that board it is composed directly, as without `$cache`); when the
content cannot be cached the `slate-debug` option says so once on the console. `$cache` pays off for a small content with costly children (shadows,
blurs, gradients, many texts) that moves often; for a long list leave it off.

## Errors

| Thrown | Message |
| --- | --- |
| `\ValueError` | `Baremetal\Slate\Scroll::$w must be between 1 and 32767` (likewise `$h`). |
| `\ValueError` | `Baremetal\Slate\Scroll::$clip is always true`. |
| `\ValueError` | `Baremetal\Slate\Scroll::$snapX must be between 0 and 32767` (likewise `$snapY`, `$snapMax`, `$contentW`, `$contentH`). |
| `\ValueError` | `Baremetal\Slate\Scroll::$scrollX must be a finite number` (likewise `$scrollY`); also from `scrollTo()` for a `$x` (`$y`) that is not finite. |
| `\ValueError` | `Baremetal\Slate\Scroll::$scrollX cannot be animated; use scrollTo()`: `animate()` or `ease()` of `$scrollX`, `$scrollY`, `$contentW`, `$contentH`, `$snapX`, `$snapY`, `$snapMax`. Other properties of a scroll that cannot be animated (`$axis`, `$bounce`, `$indicators`, `$clip`): `Baremetal\Slate\Scroll::$axis cannot be animated`. |
| `\ValueError` | `ms must be 0 or more`, `ms must be at most 2147483647` (from `scrollTo()`). |
| `\TypeError` | A value of the wrong type (PHP's own message for typed properties: an `int` for `$snapX`, a `ScrollAxis` for `$axis`). |
| `\RuntimeException` | `Slate: out of memory for the scroll`; `Slate: cannot raise Baremetal\Slate\ScrollEnded: too many event sources` (when the first scroll is drawn). |

Positions out of the limits are not errors: they are brought within them. In a class that extends `Scroll` the
messages name that class.
