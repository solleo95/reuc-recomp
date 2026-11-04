# Two players on one computer: guide

**English** · [Русский](local-coop.ru.md)

Two people can play the game together on one computer: Windows, and Linux with the desktop menu. It does not work on the Steam Deck (its own menu and Game Mode are unchanged) or during online play (see [Multiplayer](../README.md#multiplayer-online-co-op) for that).

You need two devices that can play: a keyboard and mouse is one, and every gamepad, mouse, light gun and Wii Remote is another. Any mix works, including two of the same kind.

## Quick start

1. Start the game and play as usual with your device. The first device you use is Player 1's.
2. Plug in or switch on the second player's device (a gamepad, a mouse, a light gun or a Wii Remote).
3. While a **menu** is open (the title screen, the mission menus or the in-game settings menu, **Insert** by default), press a button on the second device. A stick pushed past half, or a trigger pulled, counts too.
4. The game shows a window: *A new device was used. Who will play with it?* Choose **Player 2 joins with it**.
5. Start or continue the mission. The second player now plays with that device, with their own crosshair.

The game takes a second player **only from its menus**, never in the middle of a mission. The very first mission-time press on an unused device just plays for Player 1, as it always did.

## The new device window

| Answer | What happens |
|---|---|
| **Player 1 uses it** | You switch to that device. It is Player 1's and is not asked about again. |
| **Player 2 joins with it** | The device becomes the second player's. If a second player is there already, the window says so and replaces them. |
| **Not now** (or **Esc**) | Nothing is decided. Until the next press asks again, the device does nothing. |

- Until you answer, the new device plays for nobody, so it cannot move your crosshair by accident.
- The very first device you use, and your own mouse when you play with the keyboard, are Player 1's without a question. A keyboard is always Player 1's.
- If the question opened the menu by itself, only the window is shown and the menu closes after your answer.

## Which devices to pick

| Player 1 | Player 2 | Notes |
|---|---|---|
| Keyboard and mouse | Gamepad | The simplest. Player 2's gamepad aims with the left stick (or the gyro if you turn it on). |
| Gamepad | Gamepad | Both pads work at once. Press a button on the second pad in a menu. |
| Mouse | Mouse | Two mice, each aims on its own. See [Two mice and light guns](#two-mice-and-light-guns). |
| Mouse or gamepad | Wii Remote | Point the remote at the sensor bar (or use a Mayflash Dolphin Bar in mode 4). |
| Wii Remote | Wii Remote | The remotes' LEDs show Player 1 (LED 1) and Player 2 (LED 2). |
| Light gun | Light gun, mouse or gamepad | See the light gun notes below. |

## Controls of the second player

By default Player 2 has the standard controls of the device in their hands. To change them open the in-game menu:

- **Controls** page: choose **Player 2** in the list above the buttons. This is Player 2's mouse table (only the mouse part is used: there is one keyboard). **Restore default bindings** works for the shown player.
- **Controllers** page: Player 2 has a section of their own at the bottom with their own gamepad window (sticks, dead zones, gyro and buttons).
- Player 2's **Wii Remote** has no table of its own: it uses the buttons you set for Player 1's Wii Remote.
- The **vibration strength** slider is one setting for both players. Vibration goes to each player's own gamepad or Wii Remote.

## Taking the second player out

- **Disconnect the second player** (in the menu, **Controllers** page, the Player 2 section) removes the second player. The device then belongs to nobody: its next press asks again, and until then it plays for nobody. It does not quietly become Player 1's.
- If you do this in one of the game's own menus (not over a mission), the game goes back to one player: it stops seeing a second pad, so the second player and the second crosshair can go.
- If you do it with the settings menu open over a mission, the second pad stays (idle), because the game cannot be told mid-mission that a player left.
- If the second player's device is unplugged, the second player leaves the same way, and another device can take the place.
- Once a second player has joined in this run, the game counts as a two-player game until you restart: the same question also appears in the middle of a mission, for a device nobody has decided about (for example one that was disconnected).

## Two mice and light guns

The game reads every mouse and gun as a device of its own, so two of them do not move one cursor.

- **Windows:** mice and guns are read through Raw Input. While the second player has a mouse or a gun, the cursor does not move (the game does not need it) and Player 1's mouse is read the same way.
- **Linux:** the second player's mouse or gun is read from `/dev/input` and grabbed, so Player 1's mouse and the desktop do not see it. Only mice and guns are opened (never keyboards, touchpads, tablets or touchscreens), and nothing they send is stored or logged. You need access to `/dev/input/event*`:
  - add your user to the `input` group: `sudo usermod -aG input $USER`, then log in again; or
  - add a udev rule that gives your user access to those devices.
  Without access the game plays as before, and the menu says why the second mouse or gun is not seen.
- A light gun keeps its exact aim: its position goes through the same code as a single player's gun, from the middle of the screen to the edge. Read [Light guns and the Wii Remote](lightguns.md) for the gun settings (Sinden, GUN4IR and others).
- With two identical mice on Linux the game cannot always tell which one is Player 1's. If the wrong one asks, answer **Player 1 uses it**.

## Wii Remotes on Linux

The game uses the kernel's `hid-wiimote` driver.

1. Pair the remote over Bluetooth (press 1+2 or the red SYNC button; no PIN). It then shows up as input devices. A Mayflash Dolphin Bar in mode 4 works the same way.
2. Give your user the same `/dev/input` access as above.
3. The player LEDs show the player (LED 1 and LED 2) and the motor works through force feedback. Writing the LEDs needs write access to `/sys/class/leds`; without it they stay as the driver set them.

## Problems

- **The window never appears.** You pressed the device while the game was running a mission and no second player had joined in this run: open a menu (**Insert**, or the pause or mission menu) and press the device again.
- **The second device does nothing after joining.** Check that the device is not also grabbed by another program (Steam Input, a Dolphin Bar helper, a Sinden program). On Linux check the `/dev/input` access.
- **The window asks about the wrong device** (a mouse you use for aiming): answer **Player 1 uses it**; it is not asked about again.
- **Wrong player on a device you disconnected by mistake:** press a button on it in a menu and answer again.
- **The Steam Deck.** Local co-op is not available there.

## Checking it without the hardware (for testers)

`python -m uctest run coop` (in `scripts/`) plays a second player through the test server (`"player": 1` on any input). On Linux, `scripts/linux/uinput_fakes.py` creates virtual mice, guns and Wii Remote nodes with `uinput`, and `uc_evdev_probe` shows what the device reading sees.
