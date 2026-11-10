---
eyebrow: 'ext-slate · PHP API'
lede:    'A Slate shows one Scene at a time; a page of the application is a class that extends Scene, with its components in build() and its actions in methods and listeners.'
see_also:
  - { href: './elements.md', meta: 'API' }
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

<!-- @method name="Slate::show(Scene $s): void" returns="void" -->

Shows `$s`: `onLeave()` of the scene shown until now, `build()` the first time `$s` is shown, then
`onEnter()`. The switch is immediate and is handed to the compositor at once, with what `onEnter()` changed.
Showing the scene already shown does nothing more than hand over the pending changes. If `build()` throws, the
scene is emptied and built again by the next `show()`, and the scene shown until now stays on screen.

<!-- @method name="Slate::scene(): ?Scene" returns="?Scene" -->

The scene shown now, `null` before the first `show()`.

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
| `onEnter(): void` | Each time the scene is shown. |
| `onLeave(): void` | When another scene replaces it. |
| `onTouch(TouchEvent $e): void` | For each `Pressed`, `Moved`, `Released` and `Gesture` of the Slate's touch device while the scene is shown. |

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

## Touch

Slate forwards the touch events of its `Touch` to `onTouch()` of the scene shown when the event is delivered;
the other scenes get nothing. The events come from the event bus of ext-events, so they arrive in the
`event-driven` runtime (in `init-loop` source events are not delivered). Listeners of the same classes
registered with `Events::listen()` still get them, after the scene.

<!-- @callout variant="note" -->
This part of Slate passes the event as it is; finding which element was touched is up to the scene (compare
`$e->x` and `$e->y` with the elements' boxes).
<!-- @endcallout -->

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
