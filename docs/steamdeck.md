# Resident Evil: The Umbrella Chronicles on the Steam Deck

**English** · [Русский](steamdeck.ru.md) · [Back to the README](../README.md)

You are here: [README](../README.md) › **Steam Deck guide**

A player's guide to the native Steam Deck (Linux) version of the port: how to install it, how it is controlled, how to aim with the gyro, the trackpad, the sticks and the touch screen, what the menus hold, and what to do when something goes wrong. For the technical side of installing and releasing, see [steamdeck-install.md](steamdeck-install.md).

> [!WARNING]
> This is an early build and it has been tried on one Steam Deck only. Please report problems on [Discord](https://discord.gg/njy2CqecyY) or in the [Issues](https://github.com/solleo95/reuc-recomp/issues) (say whether you were in Game Mode or Desktop Mode, and your SteamOS version).

## Contents

- [What you need](#what-you-need)
- [Installing](#installing)
- [First start](#first-start)
- [Game Mode and Desktop Mode](#game-mode-and-desktop-mode)
- [The launcher](#the-launcher)
- [Controls](#controls)
- [Aiming](#aiming)
  - [Gyro](#gyro)
  - [Right trackpad](#right-trackpad)
  - [Sticks](#sticks)
  - [Touch screen](#touch-screen)
  - [Reload by shooting at the screen edge](#reload-by-shooting-at-the-screen-edge)
- [The in-game menu](#the-in-game-menu)
- [Graphics and performance](#graphics-and-performance)
- [Mods](#mods)
- [Where things are stored](#where-things-are-stored)
- [Updating](#updating)
- [Differences from the Windows version](#differences-from-the-windows-version)
- [Troubleshooting](#troubleshooting)
- [Uninstalling](#uninstalling)

## What you need

- A Steam Deck (LCD or OLED) with SteamOS.
- **Your own copy of the game:** a disc image of _Resident Evil: The Umbrella Chronicles_ for the Wii, region **NTSC-U, game ID `RBUE08`** (`.iso`, `.wbfs`, `.ciso`, `.wdf` or `.wia`; `.rvz` is not supported), or the game already unpacked into a folder. The port contains no game files. Only the NTSC-U release works.
- About **4 GB** of free space for the unpacked game, plus the size of the port itself. The game, the saves and the settings live next to the program, so the SD card works as well as the internal drive.

## Installing

### With the script (recommended)

In **Desktop Mode** open Konsole and run:

```bash
curl -fsSL https://raw.githubusercontent.com/solleo95/reuc-recomp/main/scripts/linux/install-steamdeck.sh | bash
```

The script downloads the newest `UC-Linux-X.Y.Z.zip`, unpacks it into `~/Games/UmbrellaChronicles`, adds **Resident Evil: The Umbrella Chronicles** to Steam with its artwork, switches Steam Input off for it and gives it a controller layout (right trackpad as a mouse, trackpad haptic feedback and trackball mode off, no gyro in the layout). Steam closes for a moment and starts again; in Game Mode the session restarts it. Run the script again to update the game: your saves, mods and game folder stay.

### By hand

1. In Desktop Mode download **`UC-Linux-X.Y.Z.zip`** from the [Releases page](https://github.com/solleo95/reuc-recomp/releases/latest) and extract it into a folder with write permission, for example `~/Games/UmbrellaChronicles`. The files go directly into that folder: `uc-launcher.sh`, `uc_launcher`, `uc`, `data` and others.
2. In Steam: **Games → Add a Non-Steam Game to My Library → Browse**, show all files and pick **`uc-launcher.sh`**.
3. Open the new entry's **Properties → Controller** and choose **Disable Steam Input** (see [Gyro](#gyro) for why).
4. Switch to Game Mode and start the game from your library.

More details and the options of the script: [steamdeck-install.md](steamdeck-install.md).

## First start

1. Start the game from Steam. `uc-launcher.sh` opens the **launcher**.
2. On the **Game** tab pick the game: **Browse** a folder with the unpacked game (the one that contains `DATA`), or **Select a disc image**. A disc image is unpacked when you press **Singleplayer** (about a minute, 4 GB of free space), into the `game` folder next to the launcher. Only an NTSC-U image is accepted; the launcher checks the disc ID first and changes nothing on a wrong image. You can delete the image afterwards.
3. Press **Singleplayer**. On the very first start the game compiles its shaders and pipelines before it shows the picture, with a progress screen. It takes longer than later starts. Do not close it.

## Game Mode and Desktop Mode

The launcher and the in-game menu have two looks:

- **Game Mode** (the Steam Deck's normal mode): the **Deck view**, made for the pad and the touch screen, with big rows and button hints.
- **Desktop Mode** (and any other Linux desktop): the same launcher and menu as in the Windows version, for mouse and keyboard. The in-game menu opens with **Insert** there.

To force one look, start with `--ui deck` or `--ui desktop`, or set the variable `UC_UI=deck|desktop`. This guide describes the Deck view.

## The launcher

The launcher has four tabs: **Game**, **Graphics**, **Controls** and **Mods**, a language select (13 languages) and the **Singleplayer** button at the bottom of the **Game** tab. Every change is saved at once.

| Button | What it does |
| --- | --- |
| D-pad up / down | Move between the rows |
| D-pad left / right | Change the focused row (slider, switch, neighbouring choice) |
| **A** | Switch a toggle, open a list or press a button |
| **L1 / R1** | Next / previous tab on the left (Game, Graphics, Controls, Mods, Language) |
| **B** | Back; on the main screen it asks whether to close the launcher and return to Steam |
| Touch | Taps and drags work everywhere |

On the **Controls** tab the **Bindings** group (below the gyro, sticks and touch aim) shows every game action with its bindings: **A** replaces a binding (then press the new button), **X** adds one, **Y** clears one. **Restore default bindings** is at the end of the list.

## Controls

The default gamepad layout of the Deck version:

| Action | Button |
| --- | --- |
| Aim | Gyro (on by default), right trackpad, left stick, or the touch screen |
| Move the camera | Right stick |
| Fire | **R2** (right trigger) |
| Interact, confirm | **A**, or a firm press of the right trackpad |
| Reload | **L2** (left trigger) |
| Knife | **B**; or hold **A** and swing the Deck with the gyro |
| Grenade | **Y**; or **A** and **R2** together |
| Previous / next weapon | D-pad up / down |
| Shake prompts (break free) | Click the right stick, or shake the Deck |
| Put the aim back in the centre | **L4** |
| Pause | **Menu** (Start) |
| Back (in menus) | **B** |
| Settings menu | **View** |

The Steam button and the **...** button open Steam's own menus on top of the game; the game gets no input meanwhile. Every action can be rebound on the **Controls** tab of the launcher or on the **Bindings** page of the in-game menu.

## Aiming

The game was made for the Wii Remote, so the pointer matters. The Deck can move it in four ways, and they work together: for example the gyro for fine aiming and the trackpad for big moves.

### Gyro

Gyro aiming is **on by default**. Tilt and turn the Deck to move the pointer. The game reads the Deck's motion sensor directly (that is why the install script switches Steam Input off for the game).

Settings, on the **Controls** tab, **Gyro** group:

- **Gyro aiming:** **Off**, **Always** (default) or **While a button is held**. With the last one a button of your choice must be held for the gyro to aim; pick the button with the **Gyro button** row (the default is **L2**, which is also Reload, so you may want another button).
- **Gyro horizontal sensitivity** and **Gyro vertical sensitivity** (10-500%, default 100%).
- **Invert gyro horizontal** and **Invert gyro vertical**.
- **Recenter aim** (default **L4**): puts the pointer back in the middle of the screen. Use it when the aim has drifted away from where you hold the Deck.

The rows for sensitivity and inversion are hidden while the gyro is off.

### Right trackpad

The right trackpad moves the pointer like a mouse. A **firm press** (harder than a touch, lighter than the physical click) counts like **A**: interact and confirm. On the **Controls** tab, in the **Touch aim** group:

- **Trackpad sensitivity** (10-300%, default 100%): how far the pointer moves for the same finger movement. Swings for the knife are not scaled.

The install script gives the game a controller layout with the right trackpad as a mouse and with the trackpad's **haptic feedback** and **trackball** mode off. If you edit the layout in Steam, the script never overwrites your changes.

### Sticks

The **left stick** aims and the **right stick** moves the camera (it is the Nunchuk's stick in the original). In the **Sticks** group:

- **Stick aim speed** (20-400%, default 120%): how fast the left stick moves the pointer. It affects the stick only, not the gyro or the trackpad.
- **Aim stick dead zone** and **Camera stick dead zone**: how far a stick must move before it counts.

### Touch screen

The touch screen is a pointer too: the pointer jumps under your finger, a **tap** interacts and a **held finger** fires. It is handy in menus.

**Aim smoothing** (**Touch aim** group) steadies a shaky pointer: higher values ease the pointer toward the new place.

### Reload by shooting at the screen edge

Set **Reload by shooting at the screen edge** to **Corner** or **Edge** and a shot near a corner or an edge becomes a reload, so you do not need a reload button. **Zone size** (1-20%) is how big that zone is. It is off by default.

## The in-game menu

Press **View** during the game. The game and the audio pause while the menu is open. Every change applies at once and is saved to `config.json`.

| Button | What it does |
| --- | --- |
| D-pad up / down | Move between the rows |
| D-pad left / right | Change the value (hold to repeat) |
| **A** | Open a list or switch a toggle |
| **B** or **View** | Close (**B** closes an open list first) |
| **L1 / R1** | Previous / next page |
| Right stick | Scroll |
| Left stick | Works like the D-pad |
| Touch | Tap a row, flip a switch, drag a slider, drag a list to scroll |

The pages: **Graphics**, **Effects**, **Controls** (gyro, sticks, touch aim), **Bindings** (the buttons), **Reticle** (aim marks), **Mods** and **Quit game**. On the last page **A** asks, and a second **A** at once ends the game.

The **Reticle** page changes the look of the aim: the wake behind the pointer, the laser dot, the white marks and the green ammo arc can each be hidden, and you can set the size, the length of the wake and the colour.

## Graphics and performance

- **Frame rate limit:** 30, 60 or 90 FPS. 90 is the maximum on Linux (the Deck OLED screen runs at 90 Hz). The limit also sets how often the game simulation steps.
- **Render scale:** the internal resolution. The default (**Monitor**) fits the screen; a lower value is faster, a higher one sharper.
- **Effects:** anisotropic filtering, per-pixel lighting, motion blur, contact shadows, bloom and its strength. Each can be switched on and off separately. Per-pixel lighting and motion blur cost the most GPU time; bloom costs very little. Ambient occlusion, ray tracing, DLSS and frame generation are not available on the Deck.
- **HDR** and **Interface brightness** are on the **Graphics** tab. HDR is on by default on Linux.
- The shaders are compiled once: the first start is slower, later starts are fast.
- Fights are heavier than quiet rooms. If the frame rate is uneven, set the limit to 60 or switch off per-pixel lighting and motion blur first.

## Mods

The **Mods** tab (in the launcher and in the in-game menu) lists the mods found in the `mods` folder next to the launcher. A mod is a folder or a `.zip` with a `mod.json`. Switch them on and off, change the load order with the arrows and open a mod's settings with its gear. What a mod draws by itself (its own tabs and live status) shows only in the game, and the in-game **Mods** page appears only while mods are enabled.

Scripts in **Lua** and **JavaScript**, texture, audio and text replacement all work on the Deck. **Native DLL mods do not exist on Linux.** Guides for making mods: [Modding](../README.md#modding).

## Where things are stored

Everything is in the install folder (`~/Games/UmbrellaChronicles` if you used the script):

| Path | What it is |
| --- | --- |
| `config.json` | All settings (the launcher and the in-game menu write it) |
| `saves/` | Your saves |
| `game/` | The unpacked game, when the launcher unpacked a disc image |
| `mods/`, `textures/` | Your mods and replacement textures |
| `data/` | The port's own data and the shader cache |
| `*.uc-backup` (in Steam's folders) | The first version of each Steam file the install script changed |

Keep the folder in a place you can write to. A software update never touches `config.json`, `saves`, `mods`, `textures` or `game`.

## Updating

When a newer `UC-Linux-X.Y.Z.zip` is the latest release, the launcher offers **Update** or **Later** at start. It downloads the archive, checks it (SHA-256) and replaces only the program files. Close the game first. The update that was interrupted (a power cut, a crash) is rolled back the next time the launcher starts. You can also run the install script again.

## Differences from the Windows version

- **Vulkan only:** no Direct3D 11 or 12, no DLSS, DLSS Frame Generation or Streamline. Options the Linux build cannot run are hidden.
- **No online co-op** on Linux yet.
- **No native DLL mods** (Lua and JavaScript mods work).
- **No native Wii Remote** support.
- **At most 90 FPS.**
- Ambient occlusion and ray tracing are not offered on the Deck.

## Troubleshooting

**The launcher says "Release has no archive for this platform".** The latest release on GitHub has no Linux archive yet. Choose **Later**; the game works normally.

**The game cannot find the game files.** On the **Game** tab choose the folder that contains `DATA`, or choose the disc image again and press **Singleplayer**. `DATA\sys\main.dol` must exist.

**The gyro does not aim.** Check that **Gyro aiming** is not **Off**, and that Steam Input is off for the game (**Properties → Controller → Disable Steam Input**). In Steam's own controller settings, **Gyroscope** must not be set to "Gyro as mouse". With **While a button is held** the chosen button must be held. If the aim drifts, press **L4**.

**The pointer jumps or the cursor moves by itself.** Almost always Steam's global **Gyro as mouse** setting: switch it off.

**The trackpad is too fast or too slow.** Change **Trackpad sensitivity** on the **Controls** tab (100% is the default).

**The trackpad vibrates or acts like a trackball.** Delete the layout folder `~/.local/share/Steam/steamapps/common/Steam Controller Configs/<your Steam ID>/config/resident evil the umbrella chronicles` (close Steam first) and run the install script again, or turn **Haptic feedback** and **Trackball** off for the right trackpad in the game's controller layout in Steam.

**The Steam entry is missing after the install script.** Run the script again from Konsole and read its messages. If Steam did not restart, start it.

**The first start takes long.** The shaders compile once. Later starts are faster.

**Not enough space.** Unpacking a disc image needs about 4 GB free. Move the folder to the SD card, or delete files you do not need.

**Everything is wrong after experiments.** Close the game and delete `config.json` in the install folder: the defaults come back at the next start. Your saves are in `saves/`.

When you report a problem, say your SteamOS version, Game Mode or Desktop Mode, the version of the port (shown in the launcher), and how to repeat it. Do not attach game files. [Reporting problems](../README.md#reporting-problems).

## Uninstalling

1. Close the game and remove the entry from the Steam library (right-click the game → **Manage → Remove non-Steam game**).
2. Delete the install folder (`~/Games/UmbrellaChronicles`). This deletes your saves too: copy `saves/` first if you want them.
3. Optionally delete the game's controller layout folder (see above) and the `*.uc-backup` copies.
