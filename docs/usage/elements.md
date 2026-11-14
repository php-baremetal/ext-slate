---
eyebrow: 'ext-slate · PHP API'
lede:    'RoundRect, Circle, Arc, Line, Text, Group, Canvas and Image: constructors, properties, colors, and a component of your own built from them.'
see_also:
  - { href: './scenes.md', meta: 'API' }
  - { href: './images.md', meta: 'API' }
  - { href: './animations.md', meta: 'API' }
  - { href: './scroll.md', meta: 'API' }
  - { href: './touch.md', meta: 'API' }
  - { href: './effects-and-options.md', meta: 'API' }
  - { href: 'package:php-baremetal/ext-display', meta: 'Extension', label: 'ext-display' }
prev: { label: 'Scenes and pages', href: './scenes.md' }
next: { label: 'Images', href: './images.md' }
---

# Elements

Every element extends the abstract `Baremetal\Slate\Element`. Its properties are public and typed: read them,
write them, use `+=` and `++` on them; each write becomes a change of the scene, shown at the next frame after
it is handed over (see [Scenes and pages](./scenes.md)).

## Common properties

| Property | Type | Meaning |
| --- | --- | --- |
| `$x`, `$y` | `int\|float` | Position relative to the parent group (the scene for a top element), -32767 to 32767. Fractions are drawn anti-aliased. |
| `$visible` | `bool` | `false` hides the element (a group with its children). Default `true`. |
| `$opacity` | `float` | 0 (transparent) to 1 (opaque). A group's opacity applies to the group as a whole. Default 1. |
| `$touchable` | `bool` | The element takes touches (`onPress()`..`onCancel()`, the callbacks of `on()`): see [Touch](./touch.md). Default `false` (`Scroll`: `true`). |
| `$hitPad` | `int` | Pixels added to the touch area on every side, 0 to 64. Default 0. |
| `$holdMs` | `int` | Milliseconds before `Hold`, 0 to 10000 (0: none). Default 500. |

<!-- @method name="Element::set(mixed ...$props): static" returns="static" -->

Several properties at once, by name: `$el->set(x: 40, opacity: 0.5)`. Positional arguments throw
`\ArgumentCountError`, an unknown name `\Error`. Returns the element. The properties are assigned in the order
given, each validated on its own (so `set(w: 40, h: 30, clip: true)` works and `set(clip: true, w: 40, h: 30)`
on a group without a box throws at `clip`; the ones before stay assigned), with PHP's usual conversions of typed
properties (a script's `strict_types` does not apply inside `set()`).

<!-- @method name="Element::animate(array $props, int $ms = 300, Ease $ease = Ease::OutCubic, int $delay = 0, int $repeat = 0, bool $yoyo = false): Tween" returns="Tween" -->

Moves properties to new values over time, in the compositor: `$el->animate(['x' => 200, 'opacity' => 0.5])`. The
properties read their final values at once; the returned `Tween` tells when it ends. See
[Animations](./animations.md), which also lists the animatable properties of each element.

<!-- @method name="Element::ease(array $props, int $ms = 300, Ease $ease = Ease::OutCubic): static" returns="static" -->

From now on, writes of the named properties slide to their values instead of jumping: `$el->ease(['x', 'y'])`;
`ease([])` makes them jump again. See [Animations](./animations.md).

<!-- @method name="Element::on(TouchType $type, \Closure $fn): static" returns="static" -->

The callback of one kind of touch (`TouchType::Tap`, ...); the element then takes touches. `off(TouchType $type)`
removes it. The methods `onPress(Touch $t)`, `onMove()`, `onRelease()`, `onTap()`, `onHold()` and `onCancel()` are
empty, for a component of your own to override. See [Touch](./touch.md).

## Positions and coordinates

Pixel (i, j) is the square from (i, j) to (i + 1, j + 1): a `RoundRect` at integer coordinates has crisp edges.
A `Line`'s end points are pixel centres: a line from (0, 10) to (100, 10) of width 1 fills row 10 exactly. Angles
are in degrees, 0 at 3 o'clock, growing clockwise (90 is 6 o'clock).

## Colors

`0xRRGGBB` is opaque; `0xAARRGGBB` with an alpha byte AA from 01 to FF is translucent (`0x80000000` is
half-transparent black). A value up to `0xFFFFFF` has no alpha byte and is opaque, so `0x00RRGGBB` cannot mean
transparent: a fully transparent fill or border is `null`, and a transparent gradient stop is written
`[position, color, 0.0]` (see [Effects and build options](./effects-and-options.md)). To hide an element, set
`$visible` to `false` or `$opacity` to 0. On the board PHP integers have 32 bits and a literal above `0x7FFFFFFF` is a float:
the color properties are typed `int|float` and accept it.

`$fill` of `RoundRect` and `Circle`, and `$color` of `Arc`, also take a `LinearGradient` or a `RadialGradient`
(see [Effects and build options](./effects-and-options.md)).

## RoundRect

<!-- @method name="RoundRect::__construct(int|float $x, int|float $y, int|float $w, int|float $h, int|float $radius = 0, int|float|Gradient|null $fill = null, int|float $border = 0, int|float|null $borderColor = null, ?Shadow $shadow = null)" -->

A rectangle with rounded corners; `$radius` 0 is a plain rectangle (clamped to half the shorter side). `$border`
is a band of that width inside the outline, in `$borderColor`; the fill covers what is left. Properties: `$w`,
`$h`, `$radius`, `$fill`, `$border`, `$borderColor`, `$shadow`.

## Circle

<!-- @method name="Circle::__construct(int|float $cx, int|float $cy, int|float $r, int|float|Gradient|null $fill = null, int|float $border = 0, int|float|null $borderColor = null, ?Shadow $shadow = null)" -->

A disc of centre (`$cx`, `$cy`), which are its `$x` and `$y`. Properties: `$r`, `$fill`, `$border`,
`$borderColor`, `$shadow`.

## Arc

<!-- @method name="Arc::__construct(int|float $cx, int|float $cy, int|float $r, int|float $from, int|float $to, int|float $width, int|float|Gradient $color, Caps $caps = Caps::Round)" -->

A stroke of `$width` along the circle of radius `$r` around (`$x`, `$y`), from `$from` clockwise to `$to`
degrees: the bar of a gauge or a progress ring. `$to` smaller than `$from` wraps round; a difference of 360 or
more is the full ring; `$from == $to` draws nothing. `$caps`: `Caps::Butt` ends on the radius, `Caps::Round`
adds half a disc. Properties: `$r`, `$from`, `$to`, `$width`, `$color`, `$caps`.

## Line

<!-- @method name="Line::__construct(int|float $x0, int|float $y0, int|float $x1, int|float $y1, int|float $width = 1, int|float $color = 0xFFFFFF, Caps $caps = Caps::Butt)" -->

A segment from (`$x0`, `$y0`), which is its `$x` and `$y`, to (`$x1`, `$y1`). The color is a plain color (no
gradient). Properties: `$x1`, `$y1`, `$width`, `$color`, `$caps`.

## Text

<!-- @method name="Text::__construct(int|float $x, int|float $y, string $text, string $font = 'default', int|float $color = 0xFFFFFF, Align $align = Align::Left, int $maxWidth = 0)" -->

One line of UTF-8 text in a font of the firmware (the fonts that ext-display builds in: `default` when
`extra.baremetal.fonts.default` is true, plus the project's fonts, see
[ext-display](package:php-baremetal/ext-display)). `$y` is the top of the line; `$align` says what `$x` is: the
left end, the centre or the right end. `$maxWidth` cuts the text to that many pixels (no wrap, no ellipsis); 0
is no limit. Extended fonts of 2 and 4 bits per pixel are drawn anti-aliased. An unknown font throws
`\ValueError` listing the fonts of the firmware. Properties: `$text`, `$font`, `$color`, `$align`, `$maxWidth`.

## Group

<!-- @method name="Group::__construct(int|float $x = 0, int|float $y = 0, int|float $w = 0, int|float $h = 0, float $opacity = 1.0, bool $clip = false, bool $cache = false, int $blur = 0, ?Shadow $shadow = null)" -->

Its children are placed relative to (`$x`, `$y`). `add(Element ...$e): static` puts children on top,
`remove(Element ...$e): static` takes them out.

| Property | Meaning |
| --- | --- |
| `$w`, `$h` | The box (0, 0, w, h) in the group's own coordinates, used by `$clip`. |
| `$clip` | Cut the children to the box. Needs `$w` and `$h` greater than 0, else `\ValueError`. |
| `$cache` | Keep the group as an image and redraw it only when something inside changes (see [Performance](./performance.md)). |
| `$blur` | Blur of the whole group in pixels, 0 to 765 (see [Effects and build options](./effects-and-options.md)). |
| `$shadow` | A drop shadow of the group's content, or `null`. |

An element is in one group at a time; a group cannot contain itself or one of its parents
(`\LogicException`).

A `Scroll` is a group with a view over its children that the finger drags, with inertia, snapping and bars: see
[Scroll](./scroll.md).

## A component of your own

The compositor never calls PHP while it draws, so a component is a `Group` that composes the built-in elements
and exposes methods that change them (a class that extends `Element` directly is refused with
`\LogicException`):

<!-- @code-block language="php" label="Gauge" -->
```php
<?php
use Baremetal\Slate\{Group, Arc, Text, Align};

final class Gauge extends Group
{
    private Arc $bar;
    private Text $label;

    public function __construct(int $x, int $y)
    {
        parent::__construct($x, $y);
        $this->add(
            new Arc(0, 0, 60, -90, 270, 8, 0x303840),
            $this->bar = new Arc(0, 0, 60, -90, -90, 8, 0xE0A040),
            $this->label = new Text(0, -8, '0', align: Align::Center),
        );
    }

    public function setValue(float $v): void       // 0..1
    {
        $this->bar->to = -90 + 360 * $v;
        $this->label->text = (string) round($v * 100);
    }
}
```
<!-- @endcode-block -->

## Canvas

<!-- @method name="Canvas::__construct(int|float $x, int|float $y, int $w, int $h)" -->

Pixels of your own, `$w` x `$h` (1 to 1024 each), opaque, drawn immediately into a buffer on the PHP side with
the primitives of ext-display. Nothing reaches the screen until `dirty()`, which sends the rectangle that
changed since the previous call. Colors are `0xRRGGBB`. Setting `$w` or `$h` resizes and clears it.

| Method | Draws |
| --- | --- |
| `clear(int $color = 0): void` | The whole canvas. |
| `pixel(int $x, int $y, int $color): void` | One pixel. |
| `line(int $x0, int $y0, int $x1, int $y1, int $color): void` | A one-pixel line, both ends included. |
| `rect(int $x, int $y, int $w, int $h, int $color): void` | An outline. |
| `fillRect(int $x, int $y, int $w, int $h, int $color): void` | A filled rectangle. |
| `circle(int $cx, int $cy, int $r, int $color): void` | An outline. |
| `fillCircle(int $cx, int $cy, int $r, int $color): void` | A filled disc. |
| `text(int $x, int $y, string $s, int $color, ?int $bg = null, string $font = 'default'): int` | One line, `$y` the top of the line; `$bg` null keeps what is under it. Returns the width. |
| `dirty(): void` | Sends the changed rectangle to the compositor. |

<!-- @code-block language="php" label="A plot" -->
```php
$plot = new Canvas(40, 200, 280, 100);
$scene->add($plot);
$plot->clear(0x101418);
foreach ($samples as $i => $v) {
    $plot->pixel($i, 99 - (int) ($v * 99), 0x40E0A0);
}
$plot->dirty();
```
<!-- @endcode-block -->

## Image

<!-- @method name="Image::__construct(int|float $x, int|float $y, string $src, float $opacity = 1.0)" -->

A picture from a file of the board, `$src` an absolute path to a `.sli` file (the project's PNG assets, converted
by phpbm) or a `.png` decoded on the board: `new Image(0, 0, '/app/assets/ui/bg.sli')`. It is loaded in the
background and drawn once it is there, at (`$x`, `$y`) rounded to whole pixels, times `$opacity`. Properties:
`$src`, and read-only `$w`, `$h`, `$loaded`; `wait(int $timeout_ms = 5000): bool` waits for it. Loading, events,
preloading and errors are in [Images](./images.md).

## Enums

| Enum | Cases |
| --- | --- |
| `Caps: string` | `Butt` (`'butt'`), `Round` (`'round'`) |
| `Align: string` | `Left` (`'left'`), `Center` (`'center'`), `Right` (`'right'`) |
| `ImageError: string` | `NotFound` (`'not-found'`), `Unreadable` (`'unreadable'`), `BadFormat` (`'bad-format'`), `Unsupported` (`'unsupported'`), `NoMemory` (`'no-memory'`), see [Images](./images.md) |
| `Ease` | `Linear`, `InQuad`, `OutQuad`, `InOutQuad`, `InCubic`, `OutCubic`, `InOutCubic`, `OutBack`, `OutBounce`, `OutElastic` (no values), see [Animations](./animations.md) |

`Caps::from('round')` and `Align::tryFrom($name)` read them from strings; `Ease` is a pure enum (`Ease::OutBack`).

## Errors

| Thrown | When |
| --- | --- |
| `\ValueError` | A property that cannot be animated, in `animate()`, `then()` and `ease()` (see [Animations](./animations.md#errors)). |
| `\ValueError` | A value out of range: `Baremetal\Slate\RoundRect::$opacity must be between 0 and 1`, `...::$hitPad must be between 0 and 64`, `...::$holdMs must be between 0 and 10000`, `...::$w must be between 0 and 32767`, `...::$x must be a finite number between -32767 and 32767`, `...::$fill must be a color, 0xRRGGBB or 0xAARRGGBB, or a gradient`, `...::$clip needs $w and $h greater than 0 (the clip box)`, an unknown font, an image path (`Baremetal\Slate\Image::$src must be an absolute path`, `...::$src must end in .sli or .png`). The old value stays. |
| `\TypeError` | A value of the wrong type (PHP's own message for typed properties). |
| `\LogicException` | A second parent, or a group that would contain itself. |
| `\RuntimeException` | The compositor failed (`Slate: the panel did not finish a band within 2000 ms`, `Slate: the panel refused the bands (error 0x..)`) or is out of memory for the changes. |
| `\Error` | A write of a read-only property (`Image::$w`, `$h`, `$loaded`), `unset()` of a property, a reference to one (`Cannot take a reference to ...`), `foreach` by reference over an element (`Cannot iterate ... by reference`), `clone` of an element, `set()` with an unknown name. |

Write properties directly or with `set()`. `array_walk()`, `ArrayObject` and `ArrayIterator` over an element write
its properties without these checks: the compositor then refuses the change when the element is shown (with
`slate-debug`, it counts as a command error) and the element keeps drawing as before.
