# Controls

Every keyboard and mouse control is an *action* with one binding: a key or a mouse button, optionally with required modifiers
(Shift, Cmd, Ctrl, Option). Edit them in the launcher (*Controls* tab): each action has a drop-down to press a key, choose a mouse button, toggle
required modifiers, clear it or restore its default. When several bindings share a key or button, the one that requires the most modifiers wins:
that is why Shift + left click is Attack 2 and plain left click stays Attack 1.

| Action | Default |
| --- | --- |
| Move | `W` `A` `S` `D` |
| Walk (hold) | `Shift` |
| Jump | `Space` |
| Interact / use object | `E` |
| Attack 1 | left click |
| Attack 2 | `Shift` + left click |
| Shield (with attack = shield bash 1; with attack 2 = shield bash 2) | right click |
| Rage move 1 / 2 | `Cmd` + left click / `Cmd` + `Shift` + left click |
| Roll | shield + jump + arrow key (rolls toward the arrow) |
| Accept | `Enter` (also sends Start on the title screen and flat menus) |
| Start button itself | `Shift` + `Enter` |
| Pause menu / back | `Esc` (pause during play, back in menus) |
| Back button | `Tab` |
| D-pad | arrow keys (`WASD` also steer menus) |
| Black / White / right trigger | `Z` / `C` / `Q` |

**If Enter does not respond** (it sometimes does not on menus, in cutscenes or on the death screen), press `Esc`: it sends Start during a scene and goes back in menus.

Not rebindable: mouse movement turns the camera (left/right only), the scroll wheel zooms (up = in).

## In game

| Key | |
| --- | --- |
| `F1` / `F2` | previous / next resolution (Original, 720p, 1080p, 1440p, 2160p) |
| `F3` | Metal performance HUD on/off |
| `F10` | full screen |
| `Cmd+Q` | quit |

On a Mac the F row sends media functions unless you hold `fn` (or enable *Use F1, F2, etc. keys as standard function keys* in System Settings).
The mouse is captured while the window has focus and released when you switch away.

Mouse sensitivity is set in the launcher (the slider; 100% is the original 1/30 of full stick per mouse pixel).

## Game controllers

Any controller macOS supports works (Xbox, DualShock 4, DualSense, Switch Pro, MFi) over USB or Bluetooth, and can be plugged in or removed while the
game runs: the game sees the insertion and removal like on the console. Up to four are used; controller 1 plays and shares port 1 with the keyboard and
mouse (both work at once: buttons are combined, and for each stick whichever is pushed further wins). Further controllers appear as ports 2-4.

Default layout (names are positions on an Xbox pad; PlayStation: Cross, Circle, Square, Triangle, L1/R1, Options, Create/Share):

| Pad input | Game button |
| --- | --- |
| A / B / X / Y (bottom / right / left / top) | A / B / X / Y (analog pressure where the pad has it) |
| LB / RB (bumpers) | Black / White |
| LT / RT (triggers, analog) | Left / right trigger |
| Menu (Start / Options) | Start |
| View (Back / Create / Share) | Back |
| Left / right stick click | Left / right thumb |
| D-pad | D-pad |
| Left / right stick | Move / camera |

The launcher's *Controller* tab changes every row (pick another input from the drop-down, or *Press a button...* and press it on the pad), and sets:
rumble on/off and strength (the game sends heavy-motor pulses when you are hit; the right motor is never used), an extra dead zone per stick (the game has
its own, so the default is just 5% against drift), camera stick sensitivity, vertical inversion, the point at which a trigger counts as a button press, and a
Nintendo-style A/B and X/Y swap. Settings are saved in `stw_settings.txt` (`pad_*` lines).
