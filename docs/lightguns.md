# Light guns and the Wii Remote

The game has two ways to take a pointing device:

- **Mouse** (default). The game reads raw mouse motion, like any shooter, and adds it up. A light gun does not work well this way: it tells Windows *where on the screen* it points, and a raw-motion reader turns that into jumps that are mostly thrown away, so the crosshair drifts like a trackball.
- **Light gun (absolute pointer).** The game follows the position of the Windows cursor inside its window. Whatever moves the cursor to the place you aim at is enough: Sinden, GUN4IR, Retro Shooter Reaper, a Mayflash Dolphin Bar in a mouse mode, a tablet.

Switch it in the launcher (**Controls** page, **Pointing device**) or in the in-game menu (**Controls** tab). It applies at once; close the menu and the game takes the cursor.

In light gun mode the cursor is hidden and stays inside the game window, the mouse sensitivity slider does nothing, and **Aim smoothing** steadies a pointer that jitters (infrared guns do). Start with 0% and raise it only if the crosshair shakes.

## Reload by shooting at the edge of the screen

A light gun has no "off-screen reload". The game offers a replacement, in the same settings: **Reload by shooting at the screen edge** with **Corner** (a shot in a corner reloads) or **Edge** (a shot near any edge reloads), and the **Zone size** (1-20% of the window, 5% by default). The shot is not fired: the game sees a reload. It works in the mouse mode too and only while playing. It is off by default, because an enemy standing in the corner would not get your shot.

If your gun sends its own "off-screen" button (Sinden and GUN4IR can send a right click), bind **Reload** to that button on the Controls page instead.

## Per device

| Device | Mode to use | Notes |
|---|---|---|
| Sinden Lightgun | Light gun | Use windowed or borderless mode: the border the Sinden program draws over the screen must stay visible. Calibrate in the Sinden program. |
| GUN4IR | Light gun | Mouse mode of the gun. |
| Retro Shooter Reaper | Light gun | Keep the gun in mouse and keyboard mode (the default); calibrate with the maker's program. |
| Mayflash Dolphin Bar, mode 1 or 2 | Light gun | The Wii Remote's pointer moves the cursor. |
| Mayflash Dolphin Bar, mode 3 | (gamepad) | Looks like a gamepad: no pointer. |
| Mayflash Dolphin Bar, mode 4 | Wii Remote | See the Wii Remote part below. |

Gun buttons arrive as mouse buttons (the trigger is usually the left button, which the game binds to **Interact**; the right button fires by default). Rebind them on the **Controls** page: put **Fire** on the trigger and **Reload**, **Knife** and **Grenade** on the other buttons.

Two guns at the same time are not supported: the game has one local player.

This mode was written from the makers' documentation and could not be tried with real guns. Please report what works and what does not.

## Wii Remote

The game also talks to a Wii Remote directly: the pointer from its infrared camera, the buttons, a Nunchuk, and swings and shakes.

**Connecting**
- Bluetooth: in Windows, *Add a device*, hold **1** and **2** on the remote (or press the red SYNC button in the battery compartment) and choose to connect without a code. Windows shows it as an input device. The remote's light stays on while it is connected.
- Mayflash DolphinBar: set it to **mode 4** (the bar's MODE button; LED 4) and sync the remote with the bar's SYNC button.
- You need a sensor bar for the pointer (the DolphinBar has one built in; otherwise the Wii sensor bar, centred above or below the screen).
- Only one program can hold the remote. Close Dolphin, Steam Input or any other tool that uses it.

**In the game.** **Controllers** page, the **Wii Remote** button: the connected remote, a switch, pointer smoothing, how strong a swing must be, the Nunchuk stick's dead zone and every button. The on-screen prompts show the remote's buttons when it was the last device you used.

**Layout by default.** Point to aim; **B** (the trigger) fires, **A** interacts, **1** reloads, **2** throws a grenade, **+** and **-** (or the D-pad left and right) change weapon, **Home** pauses; with a Nunchuk the stick moves the camera, **Z** is the knife and **C** reloads. Hold **A** and swing the remote for the knife, shake the remote for the shake prompts. In the game's menus **A** confirms and **B** goes back.

**Not supported:** the Motion Plus gyro, rumble, the speaker, a second remote, the Classic Controller.

The Wii Remote code was written from the Wii Remote's published protocol and tested with a simulated remote only, not with real hardware. The signs of the swing directions and the pointer range may need a small correction: please report what you see.

## If something does not work

**Light gun**
- The crosshair drifts or jumps like a trackball: **Pointing device** is still on **Mouse**. Switch it to **Light gun (absolute pointer)**.
- The crosshair does not match where the gun points: calibrate in the gun's own program, and use windowed or borderless mode, not exclusive fullscreen. The game follows the Windows cursor, so if the cursor is in the right place, the crosshair is too.
- The crosshair shakes: raise **Aim smoothing** a little at a time.
- A shot in the corner does nothing or fires: check that **Reload by shooting at the screen edge** is on and make the zone bigger. The mode works only while playing, not in menus.
- The cursor is stuck in the window: that is the light gun mode. Switch back to **Mouse** to get the normal cursor, or press **Alt** in windowed mode.

**Wii Remote**
- The launcher says the remote is not connected: check that Windows lists it as an input device (Settings, Bluetooth and devices) and that its light is on. Press any button to wake it. The game looks for the remote again every couple of seconds, so it can be connected while the game runs.
- "The Wii Remote cannot be used": another program holds the remote (Dolphin, Steam Input, a DolphinBar helper). Close it. Some Bluetooth adapters and drivers do not talk to the remote at all; a DolphinBar in mode 4 works on any PC.
- Buttons work but there is no pointer: the camera needs to see the sensor bar. Put it centred above or below the screen, switch it on, point from 1 to 4 metres, and keep other bright lights and candles out of the remote's view.
- The pointer shakes: raise the remote's **smoothing** in its window. Swings fire too easily or never: change the swing strength.
- The launcher reads the remote only on the **Controllers** page and only while the game is not running. When you start the game, the launcher lets the remote go.

## Settings in `config.json`

The launcher and the in-game menu write these for you; they are listed for those who edit the file by hand.

| Key | Values |
|---|---|
| `pointer` | `"relative"` (mouse, default) or `"absolute"` (light gun); command line `--pointer relative|absolute` |
| `pointer_smoothing` | 0-90 (percent), 0 by default |
| `reload_zone` | `"off"` (default), `"corner"` or `"edge"` |
| `reload_zone_size` | 1-20 (percent of the window), 5 by default |
| `wiimote` | object: `enabled`, `smoothing`, `swing_threshold`, `stick_dead_zone` and `bindings` |
