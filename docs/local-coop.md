# Two players on one computer

Two people can play the game together on one computer, on Windows and on Linux with the desktop menu. Not on the Steam Deck (its own menu and Game Mode are unchanged), and not during online play.

## How the second player joins

There is no switch to turn on, and **the game takes a second player only from its menus**: the title screen, the mission menus and the in-game settings menu. It does not take one in the middle of play.

- **Press a button on another device in a menu.** The game opens its settings menu with a window: *A new device was used. Who will play with it?* Choose **Player 1 uses it** (you switch to that device; it is not asked about again), **Player 2 joins with it** (that device becomes the second player's) or **Not now** (nothing changes; the next press on that device asks again).
- The very first device you use (and a keyboard player's own mouse) is the first player's, without a question. A keyboard is always the first player's.
- In the middle of play an unused device just plays for the first player, as it always did, and asks nothing, **unless a second player has joined in this run**: the game is then a two-player game and the same window also appears during play (for a device nobody has decided about, such as one that was disconnected).
- If a second player is there already, the window says so and **Player 2 joins with it** replaces them.
- **Disconnect the second player** (in the menu, Controllers tab) leaves the device with nobody: its next press asks again (it does not quietly become player 1's). Until it is answered the device plays for nobody. If you disconnect in a menu of the game (not over a mission), the game goes back to one player: it stops seeing a second pad, so the second player and the second reticle can go. Over a mission the second pad stays, idle.
- If the second player's device is unplugged, the second player leaves and another device can take the place. The game keeps seeing a second pad (with no buttons pressed), so a mission does not stop.

Any mix works: two gamepads, a gamepad and a mouse, a Wii Remote and a mouse, two light guns, a light gun and a Wii Remote, two Wii Remotes, and so on. Two devices of one kind work too.

## Controls of the second player

By default the second player has the standard controls of the device. You can change them in the in-game menu, **Controls** tab, with the **Player 1 / Player 2** switch: the keyboard and mouse table (only the mouse part is used: there is no second keyboard) and the gamepad table. The second player's Wii Remote has no table of its own: it uses the first player's Wii Remote controls. Vibration goes to each player's own pad or Wii Remote.

## Two mice and light guns

The game reads every mouse and gun as a device of its own, so two of them do not move one cursor.

- **Linux:** the second player's mouse or gun is read from `/dev/input` and grabbed, so the first player's mouse and the desktop do not see it. Only mice and guns are opened (never keyboards, touchpads, tablets or touchscreens) and nothing they send is stored or logged. You need access to `/dev/input/event*`: add your user to the `input` group (`sudo usermod -aG input $USER`, log in again) or add a udev rule. Without access the game plays as before and the menu says why the second mouse or gun is not seen.
- **Windows:** mice and guns are read through Raw Input. While the second player has a mouse or a gun, the cursor does not move (the game does not need it) and the first player's mouse is read the same way.
- A light gun keeps its exact aim: its position goes through the same code as a single player's gun, from the middle of the screen to the edge.

## Wii Remotes on Linux

The game uses the kernel's `hid-wiimote` driver. Pair the remote over Bluetooth (press 1+2 or SYNC; no PIN) and it shows up as input devices; a Mayflash Dolphin Bar in mode 4 works the same way. The same `/dev/input` access as above is needed. The player LEDs show the player (LED 1 and LED 2), the motor works through force feedback. Writing the LEDs needs write access to `/sys/class/leds`; without it they stay as the driver set them.

## Checking it without the hardware

`python -m uctest run coop` (in `scripts/`) plays a second player through the test server (`"player": 1` on any input). On the Steam Deck, `scripts/linux/uinput_fakes.py` creates virtual mice, guns and Wii Remote nodes with `uinput` to check the Linux device reading without the real devices.
