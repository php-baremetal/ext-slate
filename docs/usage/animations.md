---
eyebrow: 'ext-slate · PHP API'
lede:    'Element::animate() moves properties to new values over time, Element::ease() makes every write of a property slide, Slate::show() with a Transition slides or fades from one page to the next; the compositor runs them at the pace of the panel, and TweenDone and TransitionDone say when one ends.'
see_also:
  - { href: './elements.md', meta: 'API' }
  - { href: './scenes.md', meta: 'API' }
  - { href: './performance.md', meta: 'API' }
  - { href: 'package:php-baremetal/ext-events', meta: 'Extension', label: 'ext-events' }
prev: { label: 'Images', href: './images.md' }
next: { label: 'Scroll', href: './scroll.md' }
---

# Animations

An animation (a tween) moves properties of an element from the values on the screen to new ones, over a time and
along an easing curve. It runs in the compositor, not in PHP: each frame samples it at the time of the panel's
refresh (the TE pulse, or the frame's time on a panel without TE), so it stays on time when a frame is late and
never beats against a PHP timer. PHP starts it and is told when it ends; nothing in between costs PHP anything.

<!-- @code-block language="php" label="Animations" -->
```php
<?php
use Baremetal\Events;
use Baremetal\Slate\{Ease, TweenDone};

$t = $card->animate(['x' => 200, 'opacity' => 0.5], ms: 300, ease: Ease::OutCubic);
$t->then($badge, ['opacity' => 1.0], ms: 150);   // when the card has arrived

$spinner->animate(['to' => 270], ms: 800, repeat: -1, yoyo: true);   // until stopped

$knob->ease(['x', 'y'], ms: 120);                // from now on, writes of x and y slide
$knob->x = 140;

Events::listen(TweenDone::class, function (TweenDone $e) use ($t) {
    if ($e->tween === $t) {
        echo "the card is there\n";
    }
});
```
<!-- @endcode-block -->

## animate()

<!-- @method name="Element::animate(array $props, int $ms = 300, Ease $ease = Ease::OutCubic, int $delay = 0, int $repeat = 0, bool $yoyo = false): Tween" returns="Tween" -->

Moves each property of `$props` (`name => final value`) from its value on the screen to the one given, in `$ms`
milliseconds, after `$delay` milliseconds. All the properties of one call share the time and the curve and make
one `Tween`.

- `$repeat`: runs after the first; `-1` runs until it is stopped (it never ends, so it has no `TweenDone`).
- `$yoyo`: the repeats go back and forth. With an odd `$repeat` the last run comes back: the element ends where it
  started.
- The properties **read their final values at once**: `$card->x` is 200 right after the call above, while the card
  is still on its way. The values are checked like a write of the property (range, type, the rules between
  properties), before anything changes.
- A new `animate()` of a property that is already moving takes it over from where it is; the older tween goes on
  with its other properties and still ends.
- A write of a property while it moves stops that property where the write puts it (the tween goes on with the
  others); with `ease()` on the property, the write slides there from where it is.
- The animation starts when the changes are handed to the compositor (at the return to the event loop, at
  `Slate::show()` or `Slate::commit()`), like any other change.

Animatable properties, numbers and solid colours:

| Element | Properties |
| --- | --- |
| every element | `x`, `y`, `opacity` |
| `Group` | `w`, `h`, `blur` |
| `RoundRect` | `w`, `h`, `radius`, `border`, `fill`, `borderColor` |
| `Circle` | `r`, `border`, `fill`, `borderColor` |
| `Arc` | `r`, `from`, `to`, `width`, `color` |
| `Line` | `x1`, `y1`, `width`, `color` |
| `Text` | `color` |
| `Image`, `Canvas` | only the ones of every element |

Colours move channel by channel (alpha, red, green, blue). Gradients and `null` (no fill, no border) are not
animated: a gradient or `null` as the target, or as the value now, throws. To fade a fill in, start from a
transparent colour of the same hue (`0x00FF8000` to `0xFFFF8000`). A group's `blur` moves in whole pixels of its
box blur.

The easing curves are the cases of the enum `Ease`:

| Case | Curve |
| --- | --- |
| `Linear` | Constant speed. |
| `InQuad`, `InCubic` | Starts slowly, ends fast. |
| `OutQuad`, `OutCubic` | Starts fast, slows down to the end (`OutCubic` is the default). |
| `InOutQuad`, `InOutCubic` | Slow at both ends. |
| `OutBack` | Goes a little past the end and comes back. |
| `OutBounce` | Bounces at the end. |
| `OutElastic` | Oscillates around the end before it settles. |

## The Tween

| Tween | Meaning |
| --- | --- |
| `$done` (`bool`, read-only) | `true` once it ended by itself, after all its repeats. Updated when its `TweenDone` is delivered, or by `wait()`. |

While it runs, a `Tween` holds its element; Slate holds the `Tween` until its end, so a tween whose object you
drop still runs and still raises `TweenDone`.

<!-- @method name="Tween::then(Element $e, array $props, int $ms = 300, Ease $ease = Ease::OutCubic): Tween" returns="Tween" -->

A tween of `$e` (the same element or another) that starts at the exact end of this one, from the values on the
screen then. Called twice, the second follows the first (`$a->then(...)` and `$a->then(...)` make a chain of
three). On a tween that has already ended, or was stopped, it starts at once. The values of `$props` are checked
and read back at once, as for `animate()`.

<!-- @method name="Tween::stop(bool $toEnd = false): void" returns="void" -->

Stops the tween and the ones chained after it with `then()`, without `TweenDone`. The properties stay where they
are on the screen; with `$toEnd` they go to their final values (the end of the last run; for `repeat: -1`, the
values given). After a stop without `$toEnd` the properties still read the values asked: write them to put the
element somewhere (a write of the value they hold is sent all the same). Stopping a tween that ended does nothing.

<!-- @method name="Tween::wait(int $timeout_ms = 5000): bool" returns="bool" -->

Hands the pending changes to the compositor and blocks until the tween ends, at most `$timeout_ms` milliseconds:
`true` when it ended by itself. `false` at once for a stopped tween and for `repeat: -1`, which never ends. No
other event is delivered meanwhile, and `TweenDone` still comes after. Use it in a script without an event loop,
or for an animation that must finish before the next step.

## ease()

<!-- @method name="Element::ease(array $props, int $ms = 300, Ease $ease = Ease::OutCubic): static" returns="static" -->

From now on, every write of the named properties (`['x', 'y']`) slides to its value in `$ms` milliseconds, from
where the property is on the screen, instead of jumping. A write while it slides starts again from where it is,
towards the new value. `ease([])` makes the writes jump again. These moves raise no `TweenDone`. A gradient written
to an eased `fill` jumps. Returns the element.

## When tweens run

- Only while the element is shown: a tween of an element whose scene is not the one shown waits, with its time
  stopped, and goes on from where it was when the scene is shown again (time spent away does not count).
- An element Slate has not drawn yet (made in `build()`, or not added yet) is not shown either: its tween waits
  and starts when the element is shown, from the values it had before `animate()`. An intro animation can be
  written in `build()`; a `then()` after such a tween waits for it.
- `remove()` of the element stops its tweens, and the ones chained after them, without `TweenDone`; a `remove()`
  and `add()` in the same listener (a move to another group) keeps them running. A removed group's children keep
  theirs, waiting while they are not shown. A removed element stays where its tween left it, while its properties
  read the values asked: write them to put it somewhere.
- An element detached because its group or scene was freed (a page dropped while its animations run, even
  `repeat: -1`) has its tweens stopped too, so the element, its `Tween` and the compositor's copy go.
- When the Slate stops, every running tween is stopped (a waiting one waits for the next Slate).
- A tween on an element that is never shown stays pending until the script ends (in an event-driven script, forever), and keeps the element and its `Tween` alive: `stop()` it if the element may never be shown.
- `wait()` of a tween that waits (its scene not shown) waits for it, up to its timeout.
- A screen with tweens running composes a frame at each refresh of the panel; when the last one ends, the screen
  is still again and no frame is composed.
- During a page transition the tweens of both scenes run; at its end those of the scene left wait.

## Page transitions

`Slate::show($scene, $t)` with a `Transition` changes the page with an animation that the compositor runs like a
tween: the two scenes are composed together, moved (`push`, `cover`, `uncover`, towards a `Direction`) or blended
(`fade`), along the same easing curves, 300 ms with `Ease::OutCubic` by default.

<!-- @code-block language="php" label="A page transition" -->
```php
<?php
use Baremetal\Slate\{Scene, Text, Transition, Direction};

final class Menu extends Scene
{
    private Text $title;

    protected function build(): void
    {
        $this->add($this->title = new Text(180, 60, 'Menu'));
    }

    public function onEnter(): void
    {
        // runs when the transition starts: the menu's own tweens run with it
        $this->title->opacity = 0.0;
        $this->title->animate(['opacity' => 1.0], ms: 400);
    }
}

$slate->show($menu, Transition::push(Direction::Left));
```
<!-- @endcode-block -->

`onEnter()` of the new scene runs at the start, `onLeave()` of the old one and the event `TransitionDone` at the
end; the touch is ignored meanwhile. The rules, the four kinds and `show()` during a transition are in
[Scenes, Transitions](./scenes.md#transitions).

## TweenDone and TransitionDone

An event of ext-events (a final class that extends `Baremetal\Event`), delivered like the others, after the return
to the event loop (in the `event-driven` runtime):

| Event | Properties | When |
| --- | --- | --- |
| `TweenDone` | `Tween $tween` | A tween ended by itself, after all its repeats: once per tween. Never for `repeat: -1`, nor after a `remove()`; not after `stop()` either, unless the tween ended in the very frame the stop reached the compositor (then `$done` is `true` too). |
| `TransitionDone` | `Scene $from`, `Scene $to` | A transition of `Slate::show()` ended (also when a `show()` during it ended it), after `onLeave()` of `$from`. None for a transition from no scene. |

The `$done` of a tween is set also when its event cannot be delivered: when a burst of tweens ending together
overflows the event pool of ext-events, and in `init-loop`, which delivers no events, the tweens are marked done at
the return to the loop.

## Errors

Everything is checked before anything changes: on an error no property is written and nothing is sent.

| Thrown | Message |
| --- | --- |
| `\ValueError` | `Baremetal\Slate\<Class>::$<name> cannot be animated`: a property that is not in the table above (`Text::$text`, `Group::$clip`) or does not exist; also from `ease()`. |
| `\ValueError` | `Baremetal\Slate\<Class>::$fill cannot be animated: it is a gradient` (likewise `$color` of an `Arc`): a gradient as the target or as the value now. |
| `\ValueError` | `Baremetal\Slate\<Class>::$fill cannot be animated: it is null` (likewise `$borderColor`): no fill or no border, as the target or as the value now. |
| `\ValueError` | `ms must be at least 1`, `delay must be 0 or more`, `repeat must be -1 or more`, `props must name at least one property`; `ms must be at least 1` also from the `Transition` constructors. |
| `\ValueError` | `ms must be at most 2147483647`, `delay must be at most 2147483647`, `repeat must be at most 2147483647` (`ms` also from the `Transition` constructors). |
| `\ValueError` | A value out of the property's range, with the property's own message (`Baremetal\Slate\RoundRect::$opacity must be between 0 and 1`, `...::$clip needs $w and $h greater than 0 (the clip box)`). |
| `\TypeError` | A value of the wrong type (PHP's own message for typed properties). |
| `\ValueError` | `Baremetal\Slate\Tween::wait(): Argument #1 ($timeout_ms) must be greater than or equal to 0`. |
| `\RuntimeException` | `Slate: cannot animate: the table of ext-events' idle hooks is full` (from `show()` with a transition: `Slate: cannot run a transition: ...`): other extensions took the four hooks ext-events keeps (Slate uses one, for the Slate, its images, its animations and its transitions). |
| `\RuntimeException` | `Slate: cannot run a transition: the table of ext-events' watches is full` (from `show()` with a transition): other extensions took the eight watches ext-events keeps (Slate uses up to six: four for the touch, one for `Hold`, one for `TransitionDone`). `Slate: out of memory for the transition`: see [Scenes, Transitions](./scenes.md#transitions). |
| `\Error` | `new Tween()` (a `Tween` comes from `animate()` and `then()`), a write of `$done`, `clone` of a `Tween`; `new Transition()` and `clone` of a `Transition` (it comes from `fade()`, `push()`, `cover()`, `uncover()`). |
