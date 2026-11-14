---
eyebrow: 'ext-slate · PHP API'
lede:    'An element with $touchable (or a callback of on()) takes the finger: Press, Move, Release, Tap, Hold and Cancel reach its methods onPress()..onCancel() and its callbacks, on the area the finger sees on the screen; the touches that hit nothing still go to Scene::onTouch().'
see_also:
  - { href: './elements.md', meta: 'API' }
  - { href: './scenes.md', meta: 'API' }
  - { href: './scroll.md', meta: 'API' }
  - { href: 'package:php-baremetal/ext-input', meta: 'Extension', label: 'ext-input' }
  - { href: 'package:php-baremetal/ext-events', meta: 'Extension', label: 'ext-events' }
prev: { label: 'Scroll', href: './scroll.md' }
next: { label: 'Effects and build options', href: './effects-and-options.md' }
---

# Touch

Slate routes the touch events of its touch device (the `$touch` of `new Slate($display, $touch)`, or the board's
touch screen with `Slate::onboard()`) to the elements of the scene shown. An element that takes touches gets the
kinds of `TouchType` through its methods `onPress()`..`onCancel()`, which a component of your own overrides, and
through callbacks set with `on()`. The finger hits what is on the screen in the frame shown: an element that moves,
fades in or scrolls is touched where it is drawn.

<!-- @code-block language="php" label="A button with a callback, in a scene's build()" -->
```php
<?php
use Baremetal\Slate\{RoundRect, Touch, TouchType};

$ok = new RoundRect(120, 280, 120, 48, radius: 12, fill: 0x2D6CDF);
$ok->on(TouchType::Tap, fn (Touch $t) => $this->confirm());   // $this: the scene
$this->add($ok);
```
<!-- @endcode-block -->

<!-- @code-block language="php" label="A component that overrides the methods" -->
```php
<?php
use Baremetal\Slate\{Group, RoundRect, Text, Align, Touch};

final class DrinkCard extends Group
{
    public bool $touchable = true;
    private RoundRect $bg;

    public function __construct(int $x, int $y, private string $drink, private Menu $menu)
    {
        parent::__construct($x, $y);
        $this->add(
            $this->bg = new RoundRect(0, 0, 150, 90, radius: 14, fill: 0xFF141B24),
            new Text(75, 34, $drink, align: Align::Center),
        );
    }

    public function onPress(Touch $t): void   { $this->bg->fill = 0xFF1E2732; }   // pressed look
    public function onRelease(Touch $t): void { $this->bg->fill = 0xFF141B24; }
    public function onCancel(Touch $t): void  { $this->bg->fill = 0xFF141B24; }
    public function onTap(Touch $t): void     { $this->menu->choose($this->drink); }
}
```
<!-- @endcode-block -->

## Who takes a touch

| Property | Type | Default | Meaning |
| --- | --- | --- | --- |
| `Element::$touchable` | `bool` | `false` (`Scroll`: `true`) | The element takes touches. An element with at least one callback of `on()` takes them also while this is `false`. |
| `Element::$hitPad` | `int` | `0` | Pixels added to the touch area on every side, 0 to 64: a small icon easier to hit. |
| `Element::$holdMs` | `int` | `500` | Milliseconds the finger stays down before `Hold`, 0 to 10000; 0: never a `Hold`. |

A component of your own declares its own default: `public bool $touchable = true;` (likewise `$hitPad` and
`$holdMs`). None of the three can be animated. Only writes are checked: a default out of range throws nothing and
Slate uses the nearest limit (`public int $hitPad = 100;` acts as 64, `$holdMs = 20000` as 10000, a negative
value as 0), while the property still reads the value declared.

<!-- @callout variant="note" -->
Overriding a method such as `onTap()` does not make an element take touches: declare `$touchable` true (or set a
callback with `on()`) as well.
<!-- @endcallout -->

- **The area** of an element is its box on the screen in the frame shown: for a `Group`, the box of its visible
  children (without shadows and blur); for a `Group` with `$clip`, its whole clip box, children or not (an empty
  clipping group is a touch zone); grown by `$hitPad`, then cut by the clip of its ancestors (groups with `$clip`,
  the view of a `Scroll`) and by the screen.
- **The element on top wins**: the one drawn last among those whose area holds the point. A touchable child of a
  touchable group wins on its own area; the children that do not take touches are part of the area of their
  touchable ancestor (a touchable card gets the touch on its text too).
- Hidden elements (`$visible = false`, `$opacity` 0, or a hidden ancestor) take nothing.
- Elements that take no touches are transparent to the finger: a touch goes through them to the touchable element
  under them, or to the scene. To stop the touches under a dialog, make its backdrop touchable: a
  `RoundRect` with `$touchable = true` and no handler takes every touch on its area and does nothing with it.
- A `Scroll` takes touches by default: a press on its view (where no touchable child is on top) is the scroll's, and
  a drag that starts on a touchable child passes to it (see [Scrolls](#scrolls)).
- A touch whose `Pressed` hits no element goes to `Scene::onTouch()`, as before: see
  [Scene::onTouch()](#sceneontouch).

## Methods and callbacks

<!-- @method name="Element::on(TouchType $type, \Closure $fn): static" returns="static" -->

Sets the callback of one kind; one per kind: a new callback replaces the old one. The closure gets the `Touch`.
Returns the element.

<!-- @method name="Element::off(TouchType $type): static" returns="static" -->

Removes the callback of one kind (nothing when there is none). Returns the element.

| Method | Called |
| --- | --- |
| `onPress(Touch $t): void` | The finger came down on the element. From now on the touch is the element's (it captures it). |
| `onMove(Touch $t): void` | The finger moved, anywhere on the screen. |
| `onRelease(Touch $t): void` | The finger came up, anywhere on the screen. |
| `onTap(Touch $t): void` | After `onRelease()`: the finger stayed within 8 px of the press, no `Hold` came, and it came up on the area. |
| `onHold(Touch $t): void` | The finger stayed down `$holdMs` milliseconds without moving past 8 px. Once; no `Tap` after it. |
| `onCancel(Touch $t): void` | The touch ended without `Release` or `Tap` (see [Cancel](#cancel)). |

The six are public and empty in `Element`. With a method and a callback for the same kind, the method runs first,
then the callback. Both run on the PHP task, while the event of ext-input is delivered (`event-driven` runtime),
like `Scene::onTouch()`; an exception thrown by one stops there and propagates as one of `Scene::onTouch()` does.

### Touch

`final class Touch`, read-only properties:

| Property | Type | Meaning |
| --- | --- | --- |
| `$type` | `TouchType` | `Press`, `Move`, `Release`, `Tap`, `Hold` or `Cancel`. |
| `$element` | `Element` | The element that has the touch. |
| `$x`, `$y` | `int` | The finger on the screen. |
| `$localX`, `$localY` | `int` | The finger from the element's (x, y) on the screen, with the positions of the `Scroll`s it is in (the centre of a `Circle` or an `Arc`, the first end of a `Line`). While the element is off the screen: from where it was last seen. |
| `$time` | `int\|float` | When, as `Baremetal\Event::$time`: nanoseconds on the base of `hrtime(true)`, an `int` where PHP integers are 64-bit and a `float` where they are 32-bit (the board). The `$time` of the touch event itself; for `Hold` the press time plus `$holdMs`; for `Cancel` the time of the last event. |

A `Touch` is made by Slate only: its constructor is private (`new Touch()` throws `\Error`).

## The rules of a touch

One finger at a time.

```text
 Pressed on the element ──> Press ── captured: every Move and the Release come to it, also off its area
        │
        ├─ finger still (within 8 px) for $holdMs ──> Hold (once)
        │
        ├─ Moved ──> Move  (past 8 px from the press: no Tap, no Hold any more)
        │     └─ past 8 px, before a Hold, on an axis a Scroll it is in can move ──> Cancel; the Scroll has it
        │
        ├─ Released ──> Release ──> Tap  when: within 8 px of the press, no Hold, up on the area
        │
        └─ removed / hidden / not touchable / show() / a transition ──> Cancel (no Release, no Tap)
```

- `Press`: the element hit captures the finger: `Move` and `Release` come to it wherever the finger goes.
- `Tap`: after `Release`, when the finger never moved more than 8 px from the press point (the straight-line
  distance), no `Hold` came, and the release point is in the element's area now (with `$hitPad`).
- `Hold`: once, `$holdMs` milliseconds after the press, when the finger has not moved past 8 px meanwhile. After a
  `Hold` there is no `Tap`; the touch goes on (`Move`, `Release`). When a `Move` or the `Release` comes after the
  due time before the `Hold` did (a busy PHP task), the `Hold` comes first, then that event (no `Tap`).
- During a transition (`Slate::show()` with a `Transition`) the touches are ignored, as they are by
  `Scene::onTouch()`.

### Scrolls

A `Scroll` (`$touchable` is `true` by default) takes a touch in three ways:

- **Directly**: a press on its view where no touchable child is on top. The scroll gets no `Touch`: its methods
  `onPress()`..`onCancel()` and its callbacks of `on()` are never called (a callback still makes it take touches,
  also with `$touchable = false`). Its content follows the finger past 8 px on its axis, as in
  [Scroll](./scroll.md#touch).
- **Handed over**: a touchable element inside it moves more than 8 px from the press before a `Hold`. The axis of
  the larger movement (vertical on a tie) decides: when the scroll can move on it, the element gets `Cancel` (at
  once, at the point past 8 px) and the scroll takes the touch as if it had started there, at the press point and
  time; the content follows the finger from that point, without a jump. When it cannot (a vertical list and a
  sideways move, a content that fits the view), the element keeps the finger: `Move`, `Release`, no `Tap`. The axis
  is decided once, at that point. After a `Hold`, the element keeps it as well.
- **Caught**: a press while the scroll (or a scroll it is in) moves stops it; the touch is that scroll's, and the
  element under the finger gets nothing, not even `Press`.

With scrolls inside scrolls the touch goes to the innermost one that can move in the direction of the drag: a
horizontal carousel in a vertical list takes the sideways drags, the list the vertical ones, from a card as from
the carousel's own space. The outer one takes it only when the inner one cannot move on that axis at all (locked, or
a content that fits); an inner scroll at its edge keeps the finger and stretches. A `Scroll` with
`$touchable = false` takes nothing, and its touchable children keep their touches.

A scroll's touch ends (its drag stops without speed: no inertia, a `ScrollEnded` after once a drag had started) for
the causes of a `Cancel` below, and when its `Released` is lost (a new `Pressed` comes first); the rest of the touch
reaches nobody. What the element's `onCancel()` does after a hand-off (`Slate::show()`, removing the scroll, making
it not touchable) ends the scroll's touch the same way.

### Hold

The time comes from one internal timer of ext-events, armed at the press for the element's `$holdMs` and stopped
at the release, at a cancel and at a move past 8 px; when it is due, Slate delivers `Hold` from its own watch of the
internal event `HoldDue` (no listener needed: it is not for scripts). A change of `$holdMs` applies from the next
press.

### Cancel

A touch ends with `onCancel()`, and without `Release` or `Tap`, when:

- a `Scroll` it is in takes the finger (see [Scrolls](#scrolls));
- the captured element or one of its ancestors is removed from the scene (`remove()`) or hidden
  (`$visible = false`);
- the captured element stops taking touches: `$touchable = false` with no callback left, or `off()` of its last
  callback while `$touchable` is `false`;
- `Slate::show()` shows another scene, or starts a transition.

A cancel caused inside a touch handler (a method or a callback of the element) comes right after that handler
returns, never inside it; one caused anywhere else comes at once, inside the call that caused it (`remove()`, the
write). `show()` delivers it at once, before `onLeave()` of the scene it leaves (also when it runs inside a
handler). After a cancel the rest of that touch reaches nobody, not even `Scene::onTouch()`: the next `Pressed`
starts a new one.

The touch is over once the finger comes up: whether it is a `Tap` is decided at the release, so removing or hiding
the element inside `onRelease()` gives no `Cancel` and `onTap()` still follows.

An element hidden (also with `$opacity` 0, itself or an ancestor), made not touchable or removed takes no new touch
at once, even before the next frame shows the change. `$opacity` 0 written during a touch does not cancel it. The
opacity checked is the value set, not the one drawn: an element fading out with `ease()` to 0 takes no new touch
from the write on.

## Scene::onTouch()

`Scene::onTouch()` gets the raw `TouchEvent`s (`Pressed`, `Moved`, `Released`, `Gesture`) of the touches whose
`Pressed` hits no touchable element: the background, a swipe between pages. A scene without touchable elements gets
every event, as before. A touch that an element or a `Scroll` took never reaches it, nor does its `Gesture` (the
touch controller may report the swipe after the `Released`: it is still that touch's), nor the rest of a cancelled
touch. See [Scenes and pages](./scenes.md#touch).

`Scroll::touch()` still works for a scroll the script drives from `onTouch()`: one with `$touchable = false`, or the
touches that hit nothing (see [Scroll](./scroll.md#touch-routing-by-hand)). While Slate routes a touch to a scroll,
its `touch()` does nothing and returns `false`, so no touch is fed twice.

## Costs

A scene without touchable elements costs nothing more: no table, no work per frame. With touchable elements the
compositor writes a small table of where they are after each frame that changed something (about 20 bytes per
element, in external RAM), and PHP reads it without a lock at each `Pressed`.

## Errors

| Thrown | When |
| --- | --- |
| `\ValueError` `Baremetal\Slate\<Class>::$hitPad must be between 0 and 64` | A write of `$hitPad` out of range; the old value stays. |
| `\ValueError` `Baremetal\Slate\<Class>::$holdMs must be between 0 and 10000` | A write of `$holdMs` out of range; the old value stays. |
| `\ValueError` `Baremetal\Slate\<Class>::$<name> cannot be animated` | `animate()` or `ease()` of `$hitPad`, `$holdMs` or `$touchable`. |
| `\TypeError` | `on()` or `off()` with something other than a `TouchType` (and a `\Closure` for `on()`), a value of the wrong type for a property. |
| `\RuntimeException` `Slate: cannot take the touch: the table of ext-events' watches is full` | From `new Slate()` or `Slate::onboard()` with a touch device: other extensions took the eight watches ext-events keeps (Slate uses up to six: four for the touch events, one for `Hold`, one for `TransitionDone`). |
| `\RuntimeException` `Slate: out of memory for the touch` | From `new Slate()` or `Slate::onboard()` with a touch device. |
| `\RuntimeException` `Slate: cannot raise Baremetal\Slate\HoldDue: too many event sources` | From `new Slate()` or `Slate::onboard()` with a touch device: ext-events has no room for one more event class. |
