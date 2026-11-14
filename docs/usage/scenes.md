---
eyebrow: 'ext-slate · PHP API'
lede:    'A Slate shows one Scene at a time, at once or with a transition; a page of the application is a class that extends Scene, with its components in build() and its actions in methods and listeners.'
see_also:
  - { href: './elements.md', meta: 'API' }
  - { href: './animations.md', meta: 'API' }
  - { href: './touch.md', meta: 'API' }
  - { href: '../overview.md', meta: 'Overview' }
  - { href: 'package:php-baremetal/ext-input', meta: 'Extension', label: 'ext-input' }
prev: { label: 'Overview', href: '../overview.md' }
next: { label: 'Elements', href: './elements.md' }
---

# Scenes and pages

## Slate

<!-- @method name="Slate::__construct(Display $display, ?Touch $touch = null)" -->

Takes the panel of `$display` (a `Baremetal\Output\Display` whose driver has the band path) and starts the
compositor. With `$touch` (a `Baremetal\Input\Touch`), Slate turns on its events and forwards them to the scene
shown at the time. From now on the drawing methods of the display throw `\LogicException`; `width()`,
`height()`, `backlight()`, `sleep()`, `wake()` and `refreshRate()` keep working. When the panel has a TE pin, the
frames start at its pulses. Between `sleep()` and `wake()` no new frame starts (a frame already under way at `sleep()` finishes; the changes wait for `wake()`); when the
driver re-initialises the panel on `wake()` (St77916 does), Slate sends the whole screen again.

<!-- @method name="Slate::onboard(): Slate" returns="Slate" -->

A Slate on the board's own screen. When the board has a `display` block, Slate looks among the loaded classes
for the display driver that declares a static `onboard()` (for example `St77916` of esp32-display-st77916) and
calls it; when the board also has a `touch` block and one touch driver with `onboard()` is loaded (for example
`Cst816s` of touch-cst816s), its device becomes the touch of the Slate. No display block, no display driver, or
several drivers of the same kind with `onboard()`: `\LogicException`, and the Slate is built by hand with
`new Slate($display, $touch)`.

<!-- @method name="Slate::show(Scene $s, ?Transition $t = null): void" returns="void" -->

Shows `$s`. Without `$t` the switch is immediate: `onLeave()` of the scene shown until now, `build()` the first
time `$s` is shown, then `onEnter()`. With `$t` the new scene slides or fades in (see
[Transitions](#transitions)). Either way the change is handed to the compositor at once, with what `onEnter()`
changed. Showing the scene already shown does nothing more than hand over the pending changes. If `build()`
throws, the scene is emptied and built again by the next `show()`, and the scene shown until now stays on screen.

<!-- @method name="Slate::scene(): ?Scene" returns="?Scene" -->

The scene shown now, `null` before the first `show()`. During a transition, the scene it goes to.

<!-- @method name="Slate::commit(): void" returns="void" -->

Hands the changes made so far to the compositor now. The event loop does it by itself after each listener
(`event-driven`) and after each `loop()` (`init-loop`); `commit()` is for a script that never returns to a loop.
The compositor composes the frame in its own task, while the script goes on: when the program ends, the Slate
stops, so a script that only draws must keep running (a loop, or the `event-driven` runtime) for the frame to
reach the panel.

One Slate at a time: a second `new Slate()` or `Slate::onboard()`, on the same panel or another, throws
`\LogicException` (`Slate: a Slate already drives <Class>; only one Slate can run at a time`). `unset($slate)`
(or the end of the program) stops the compositor and gives the panel back; the next `flush()` of the display
sends its whole framebuffer.

## Scene

<!-- @method name="Scene::add(Element ...$e): static" returns="static" -->

Adds elements on top of the scene's elements: the last one added is drawn on top.

<!-- @method name="Scene::remove(Element ...$e): static" returns="static" -->

Takes elements out; they stay usable and can be added again, here or elsewhere.

| Method | Called |
| --- | --- |
| `protected build(): void` | Once, by the first `show()` of the scene, before `onEnter()`: add the components here. |
| `onEnter(): void` | Each time the scene is shown; with a transition, when it starts. |
| `onLeave(): void` | When another scene replaces it; with a transition, when it ends. |
| `onTouch(TouchEvent $e): void` | For each `Pressed`, `Moved`, `Released` and `Gesture` of the Slate's touch device while the scene is shown, for the touches that no element takes (see [Touch](#touch)). |

All four are empty in `Scene`. A scene can also be built without a class:

<!-- @code-block language="php" label="A scene without a class" -->
```php
$about = (new Scene())->add(
    new Text(180, 170, 'php-baremetal', align: Align::Center),
);
$slate->show($about);
```
<!-- @endcode-block -->

## A page

A page is a class: its components are properties built in `build()`, its actions are methods, and listeners
change it.

<!-- @code-block language="php" label="Two pages" -->
```php
<?php
use Baremetal\Input\{TouchEvent, Gesture, GestureType};
use Baremetal\Slate\{Slate, Scene, RoundRect, Text, Align};

final class Home extends Scene
{
    public Text $temp;

    protected function build(): void
    {
        $this->add(
            new RoundRect(20, 20, 320, 120, radius: 16, fill: 0x202830),
            $this->temp = new Text(180, 60, '-- °C', align: Align::Center),
        );
    }

    public function setTemperature(float $c): void
    {
        $this->temp->text = sprintf('%.1f °C', $c);
    }

    public function onTouch(TouchEvent $e): void
    {
        global $slate, $settings;
        if ($e instanceof Gesture && $e->type === GestureType::SwipeLeft) {
            $slate->show($settings);
        }
    }
}

final class Settings extends Scene
{
    protected function build(): void
    {
        $this->add(new Text(180, 170, 'Settings', align: Align::Center));
    }
}

$slate = Slate::onboard();
$home = new Home();
$settings = new Settings();
$slate->show($home);
```
<!-- @endcode-block -->

## Transitions

<!-- @code-block language="php" label="Transitions" -->
```php
<?php
use Baremetal\Events;
use Baremetal\Slate\{Transition, Direction, TransitionDone, Ease};

$slate->show($menu, Transition::push(Direction::Left));                    // forward
$slate->show($home, Transition::push(Direction::Right));                   // back
$slate->show($panel, Transition::cover(Direction::Up, ms: 250));           // a panel slides up over the page
$slate->show($home, Transition::uncover(Direction::Down));                 // and slides away again
$slate->show($splash, Transition::fade(ms: 400, ease: Ease::InOutCubic));
$slate->show($home);                                                        // at once

Events::listen(TransitionDone::class, function (TransitionDone $e) {
    echo get_class($e->from), ' -> ', get_class($e->to), "\n";
});
```
<!-- @endcode-block -->

<!-- @method name="Transition::fade(int $ms = 300, Ease $ease = Ease::OutCubic): Transition" returns="Transition" -->

The new scene appears over the old one, its opacity going from 0 to 1.

<!-- @method name="Transition::push(Direction $d, int $ms = 300, Ease $ease = Ease::OutCubic): Transition" returns="Transition" -->

The new scene comes in from the side opposite to `$d` and pushes the old one out towards `$d`:
`Direction::Left` is the usual "forward", `Direction::Right` "back".

<!-- @method name="Transition::cover(Direction $d, int $ms = 300, Ease $ease = Ease::OutCubic): Transition" returns="Transition" -->

The new scene slides in towards `$d` over the old one, which stays still.

<!-- @method name="Transition::uncover(Direction $d, int $ms = 300, Ease $ease = Ease::OutCubic): Transition" returns="Transition" -->

The old scene slides away towards `$d` and uncovers the new one, which stays still.

`Direction` is an enum: `Left`, `Right`, `Up`, `Down`, the direction of the movement. `$ease` is one of the
[easing curves](./animations.md) of the animations. A `Transition` is a value: keep one and pass it to as many
`show()` calls as you like. `$ms` below 1 throws `\ValueError` `ms must be at least 1`, above 2147483647
`ms must be at most 2147483647`.

What happens, in order:

1. `show($b, $t)`: `build()` of `$b` the first time, then `onEnter()` of `$b` at once: its elements are ready and
   its tweens start with the transition. `scene()` is `$b` from now on.
2. For `$ms` milliseconds the compositor composes both scenes together, band by band, at the pace of the panel.
   The tweens of both scenes run. The touch of the Slate reaches neither scene: a touch during a transition is
   lost.
3. At the end: `onLeave()` of the old scene, then the event `TransitionDone` with `$from` (the old scene) and
   `$to` (the new one), a final class that extends `Baremetal\Event`, delivered after the return to the event
   loop. The old scene's tweens wait, as for any scene that is not shown.

- `show()` of the scene shown, or of the one a transition goes to, does nothing.
- `show()` during a transition ends it first: `onLeave()` of the scene it leaves runs at once, its
  `TransitionDone` follows; then the new change starts from the scene the first one went to, whole. Going back to
  the scene being left is fine: `onLeave()` runs for it, then `onEnter()` again.
- The first `show()` with a transition (no scene shown before) brings the scene in over the black background.
  Nothing is left, so no `TransitionDone` comes.
- In `init-loop`, which delivers no events, there is no `TransitionDone`, and the `onLeave()` of the old scene
  runs at the next `show()` or `commit()` after the end: a `$slate->commit()` in `loop()` runs it promptly. The
  same happens in `event-driven` when the event pool of ext-events is full.
- When the Slate stops during a transition (`unset($slate)`), neither `onLeave()` nor `TransitionDone` comes.
- When memory is too short for the transition's layers, the change is a cut (at once), still followed by
  `onLeave()` and `TransitionDone`.

| Thrown by `show()` with a transition | When |
| --- | --- |
| `\RuntimeException` `Slate: cannot run a transition: the table of ext-events' idle hooks is full` | Other extensions took the four idle hooks ext-events keeps (Slate uses one, shared by all its parts). |
| `\RuntimeException` `Slate: cannot run a transition: the table of ext-events' watches is full` | Other extensions took the eight watches ext-events keeps (Slate uses up to six: four for the touch, one for `Hold`, one for `TransitionDone`). |
| `\RuntimeException` `Slate: out of memory for the transition` | No memory for the few bytes that carry its end. Nothing changed. |

A frame during a transition costs about as much as the two scenes together, and the whole screen is sent at
each refresh while the scenes move. The cached groups of both scenes are used and move with their scene. See
[Performance](./performance.md).

## Returning to a scene

A scene left keeps its caches (cached groups, shadows, blurs) while there is room, so showing it again, with or
without a transition, draws it from those images at once instead of building them again.

<!-- @code-block language="php" label="Kept caches" -->
```php
$slate->keepScenes = 2;     // scenes left that keep their caches (default 2; 0: none)
$slate->keptBudget = null;  // a limit in KB for those caches (default null: only the cache budget)

final class Splash extends Scene
{
    protected bool $keepCache = false;   // shown once: its caches go when it is left
}
```
<!-- @endcode-block -->

| Property | Type | Default | Meaning |
| --- | --- | --- | --- |
| `Slate::$keepScenes` | `int` | `2` | How many scenes left keep their caches. Past it, the scene left first drops them. `0`: every scene left drops its caches, as in 1.1. |
| `Slate::$keptBudget` | `?int` | `null` | A limit in KB for the caches of the scenes left; past it, the scene left first drops them. `null`: no limit of its own (only the cache budget); `0`: nothing is kept. |
| `Scene::$keepCache` | `bool` (protected) | `true` | `false`: the scene drops its caches when it is left. Read when the scene is left. |

- The kept caches stay where they are: in Slate's cache budget, in external RAM (PSRAM). They take no internal
  RAM and no memory beyond that budget.
- When the scene shown, or the underlay, needs room in the budget, the scenes left drop their caches first, the
  one left first before the others: a kept cache never takes room from the scene on the screen.
- Images are not part of the budget, but when an image cannot be loaded for lack of memory, the scenes left drop
  their caches and the image is loaded once more; only then does it fail with `no-memory`.
- Lowering `$keepScenes` or `$keptBudget` drops the extra caches at once.
- A scene whose caches were dropped builds them again at its first frame, as before.

| Thrown by a write | When |
| --- | --- |
| `\ValueError` `keepScenes must be 0 or more` | A negative `$keepScenes`; the old value stays. |
| `\ValueError` `keptBudget must be 0 or more` | A negative `$keptBudget`; the old value stays. |

Like the elements' properties, they cannot be taken by reference or unset (`\Error`).

## Touch

Slate routes the touch events of its `Touch` when the event is delivered: a `Pressed` on an element that takes
touches (`$touchable`, or a callback of `on()`) makes the whole touch that element's (`onPress()`..`onCancel()`,
see [Touch](./touch.md)); a `Scroll` takes touches by default (see [Scroll](./scroll.md#touch)); the touches whose `Pressed` hits no such element go to `onTouch()` of the scene shown, as
the raw `TouchEvent`s (`Pressed`, `Moved`, `Released`, `Gesture`). A `Gesture` the touch controller reports after
the `Released` belongs to that touch: the scene gets it only when the touch was the scene's (a swipe that drags a
slider or a carousel is not a page change). A scene without touchable elements gets every event. The other scenes get nothing. The events come from the event bus of ext-events, so they arrive in the
`event-driven` runtime (in `init-loop` source events are not delivered). Listeners of the same classes
registered with `Events::listen()` still get them all, after Slate. During a transition neither the elements nor
the scenes get them (the listeners still do).

`show()` of another scene (and the start of a transition) ends a touch an element holds: the element gets
`onCancel()` before `onLeave()` of the scene left (a `Scroll` that holds one stops its drag without speed).

## Lifetime

- A group (or a scene) keeps its children alive; the Slate keeps the scene it shows, the display and the touch
  device.
- A component of your own extends `Group`: `new` of a class that extends `Element` directly throws
  `\LogicException` (`<Class>: extend Baremetal\Slate\Group to build a custom element`).
- Slate properties cannot be taken by reference (`$r = &$el->x`, a by-reference argument): `\Error`
  `Cannot take a reference to <Class>::$x`. Every write goes through validation and reaches the compositor.
- An element belongs to one parent at a time: adding it to a second one throws `\LogicException`
  (`... already belongs to a group: remove() it from there first`).
- An element dropped by PHP (no reference left) is removed from the compositor too.
- Elements can be built before the Slate exists; nothing reaches the compositor until their scene is shown.
  After `unset($slate)` a new Slate shows the same scenes again.
