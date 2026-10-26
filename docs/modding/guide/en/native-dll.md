# Native DLL mods

[Русский](../ru/native-dll.md) · **English**

You are here: [Modding guide](README.md) › **Native DLL**

How to write a mod in C or C++ as a Windows DLL: for work that scripts are too slow or too limited for, your own overlays, heavy algorithms or calling Windows. This is the most powerful kind of mod and the one that needs the most care.

## Contents

- [When to choose a DLL](#when-to-choose-a-dll)
- [What the player has to do](#what-the-player-has-to-do)
- [What you need](#what-you-need)
- [The smallest DLL](#the-smallest-dll)
- [The API table](#the-api-table)
- [Using the game API through JSON](#using-the-game-api-through-json)
- [Events](#events)
- [Hooks](#hooks)
- [Drawing with ImGui](#drawing-with-imgui)
- [Building](#building)
- [Packing a native mod](#packing-a-native-mod)
- [What the port does for you](#what-the-port-does-for-you)
- [Rules of the road](#rules-of-the-road)
- [Troubleshooting](#troubleshooting)
- [See also](#see-also)

## When to choose a DLL

Choose a script ([Lua](lua.md), [JavaScript](javascript.md)) first. They are safer, work for every player at once, and are enough for nearly everything. Choose a DLL only when you need:

- **speed** that a script's 1 ms per step cannot give (path-finding, heavy maths, big data);
- **your own drawing** with Dear ImGui beyond what the `ui` widgets offer;
- **Windows or library calls** (files, sockets, other libraries).

A DLL runs **inside the game process with the full rights of the player's account**. Players are right to be careful with it, so publish a DLL only with source code or a clear description, and expect that many players will not enable it.

## What the player has to do

The port gates DLLs in two steps; your players will see both:

1. Turn on **Allow native mods** in the Mods tab (off by default; turning it on shows a warning).
2. **Confirm** each DLL. The port stores the DLL's SHA-256 and asks again whenever the file changes.

Until both are done the DLL does not run, and the Mods tab says so. Tell your players about these steps in your mod's description.

## What you need

- A C or C++ compiler for **64-bit Windows** (Visual Studio, or clang-cl).
- The header **`uc_mod.h`**: in the `sdk` folder next to the game (and `modding/uc_mod.h` in the source repository).
- If you draw with Dear ImGui: the ImGui sources of **exactly the version named by `UC_MOD_IMGUI_VERSION`** in `uc_mod.h` (at the time of writing `1.92.9b`).
- A `mod.json` that names the DLL:

```json
{ "id": "my-native-mod", "name": "My native mod", "version": "1.0", "api": "1.1", "touches_gameplay": true, "native": "native/my_mod.dll" }
```

A mod may have both a script and a DLL.

## The smallest DLL

```c
#include "uc_mod.h"

UC_MOD_DEFINE_INFO()                       /* exports uc_mod_get_info: the API and ImGui versions of this build */

static const UcApi* g_api;

static void on_step(const char* payload_json, void* user) {
    (void)payload_json; (void)user;
    /* called about 30 times per game second */
}

UC_MOD_EXPORT int uc_mod_init(const UcApi* api) {
    g_api = api;
    api->log("my mod started");
    api->on("on_step", on_step, NULL);
    return 0;                              /* non-zero: the mod is not loaded */
}

UC_MOD_EXPORT void uc_mod_shutdown(void) {}   /* optional but recommended */
```

You export three things:

| Export | Purpose |
|---|---|
| `uc_mod_get_info` (made by `UC_MOD_DEFINE_INFO()`) | Tells the port which API version and ImGui version the DLL was built with. The port **refuses** a DLL that lacks it, was built for another API major version or a newer minor version than the game offers, or for another ImGui version. |
| `uc_mod_init(api)` | Called once when the mod starts. Keep the `api` pointer, register events and hooks. Return 0 on success. |
| `uc_mod_shutdown()` | Called when the mod stops. Free what you allocated. |

## The API table

`UcApi` is a table of function pointers. Call it **only** from `uc_mod_init` and from your callbacks, on the thread the port calls you on (the game thread for events and hooks, the menu thread for drawing).

| Member | Use |
|---|---|
| `log(line)` | Writes a line to the mod log (`debug\mods.log`). |
| `mem_read`, `mem_write` | Game memory as raw big-endian bytes. Return 1 on success. |
| `get_json`, `set_json`, `call_json` | The whole [game API](game-api.md) through JSON text. Return 0 on success. |
| `settings_get_json`, `settings_set_json` | The mod's declared settings. |
| `on(event, callback, user)` | Events (`on_step`, `on_load`, ...). The payload is JSON text. |
| `hook_pre`, `hook_replace`, `hook_post` | Hooks. |
| `set_ui(draw, user)` | A function called every frame the settings window is open. |
| `imgui_context`, `imgui_alloc`, `imgui_free`, `imgui_alloc_user` | For drawing with ImGui (filled in right before each call of your draw function). |
| `mod_id`, `imgui_version`, `api_major`, `api_minor` | Information about your mod and the build. |

Strings the port hands you (`const char** result`) stay valid **until your next API call**: copy them if you need them longer.

## Using the game API through JSON

`get_json`, `set_json` and `call_json` give a DLL the same API as scripts. The path is `domain.member`; the arguments are a JSON object (`slot`, `id`, ...):

```c
const char* out = NULL;

/* read: uc.player(1).present */
if (g_api->get_json("player.present", "{\"slot\":1}", &out) == 0) {
    /* out is "true" or "false" */
}

/* write: uc.player(1).health = 1 */
if (g_api->set_json("player.health", "{\"slot\":1}", "1", &out) != 0) {
    g_api->log(out);                      /* on failure out is the error text */
}

/* call: uc.game.give_weapon(1, 2) */
g_api->call_json("game.give_weapon", "{\"slot\":1,\"id\":2}", &out);

/* a global domain has no slot: uc.game.time */
g_api->get_json("game.time", "{}", &out);
```

They return 0 on success; then `out` is the JSON value (for `get` and `call`) or an empty string (for `set`). On failure they return non-zero and `out` is the error text. The writes need `touches_gameplay`, as in scripts.

For raw memory use `mem_read` and `mem_write`; they return 1 on success and give the bytes in **big-endian** order, exactly as the game stores them:

```c
uint8_t bytes[4];
if (g_api->mem_read(0x80400000, bytes, 4)) {
    uint32_t value = (uint32_t)bytes[0] << 24 | bytes[1] << 16 | bytes[2] << 8 | bytes[3];
}
```

## Events

`api->on(name, callback, user)` registers a callback `void callback(const char* payload_json, void* user)`. The payload is the JSON of the event: `{}` for `on_step`, `{"slot":1}` for `on_weapon_fired`, `{"id":10,"x":1.5,"y":0.0,"z":4.0}` for `on_enemy_killed`. The event names and payloads are the same as for scripts ([Game API](game-api.md#events)). You can parse the payload with any JSON library, or for a single number search the text with `strstr`.

## Hooks

```c
static void before_update(UcHookRegs* regs, void* user) {
    (void)user;
    /* regs->r[0] is r3 ... regs->r[7] is r10; regs->f[0] is f1 ... regs->f[7] is f8 */
    regs->r[1] = 0;                        /* r4 = 0: firing not allowed this step */
}

/* in uc_mod_init: */
if (api->hook_pre("GameWeaponUpdate", before_update, NULL) != 0) api->log("GameWeaponUpdate cannot be hooked");
```

`hook_replace` and `hook_post` (API 1.1) work the same; for a replacement `regs->r[0]` and `regs->f[0]` are the return value. They return 0, or non-zero when the name is not hookable. What a hook may and may not do is explained in the [Hooks guide](hooks.md).

## Drawing with ImGui

Your DLL carries its **own copy of Dear ImGui**, of exactly the version in `UC_MOD_IMGUI_VERSION`. At the start of **every call of your draw function** (not in `uc_mod_init`, because the context does not exist yet) hand over the game's context and allocator:

```cpp
static void draw(void*) {
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(g_api->imgui_context));
    ImGui::SetAllocatorFunctions(g_api->imgui_alloc, g_api->imgui_free, g_api->imgui_alloc_user);

    ImGui::TextWrapped("Hello from a native mod.");
    if (ImGui::Button("Click me")) ++g_clicks;
    ImGui::SliderFloat("a value", &g_value, 0.f, 1.f);
}

/* in uc_mod_init: */
api->set_ui(draw, nullptr);
```

and draw normal ImGui widgets. `draw` is called every frame the mod's settings window (the gear button) is open, inside an ImGui window, on the menu thread. It has a 4 ms budget.

In `uc_mod_init` check that your ImGui matches the game's, and refuse to load if not:

```cpp
if (std::strcmp(IMGUI_VERSION, UC_MOD_IMGUI_VERSION) != 0) return 1;
```

## Building

A complete, working project is the example mod `mods-examples\hello_native` (it builds with the project's CMake). Outside the repository a minimal `CMakeLists.txt` looks like this:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_native_mod CXX)
set(CMAKE_CXX_STANDARD 17)

set(IMGUI_DIR "<imgui dir>")                         # the ImGui version named in uc_mod.h
set(UC_SDK_DIR "<install dir>/sdk")             # the folder with uc_mod.h

add_library(my_mod MODULE src/my_mod.cpp
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp)
target_include_directories(my_mod PRIVATE ${IMGUI_DIR} ${UC_SDK_DIR})
set_target_properties(my_mod PROPERTIES PREFIX "" OUTPUT_NAME my_mod)   # my_mod.dll
```

A DLL that draws nothing needs neither ImGui nor its sources. Build for **x64** and copy the result to `mods\my-native-mod\native\my_mod.dll`.

## Packing a native mod

Put the DLL in the mod's `native` folder, name it in `mod.json` (`"native": "native/my_mod.dll"`), and zip the mod folder ([Testing and sharing](testing-and-sharing.md#packing-a-mod)). Mention in your description that players must turn on **Allow native mods** and confirm the DLL.

## What the port does for you

- A **hardware exception** (access violation and the like) inside a callback is caught and counts as a failure of your mod. Three failures switch the mod off. The DLL stays loaded (the port cannot make a corrupt DLL safe).
- Callbacks are **timed** after they return: more than the budget (1 ms for steps, 4 ms for drawing) counts as a failure. A native callback cannot be interrupted.
- Windows' own "bad image" error boxes are suppressed; a file that is not a DLL is refused with a message.
- When the mod is switched off the port stops calling you, calls `uc_mod_shutdown`, and takes back what you set through the API.

## Rules of the road

- Call the API **only** from `uc_mod_init` and your callbacks, never from your own threads.
- **Keep callbacks short.** Do heavy work on your own worker threads and hand results over, but never call the API from them.
- **Copy strings** you want to keep; the port's strings die at your next API call.
- **Memory:** check every pointer; a crash in your DLL takes the whole game with it.
- Build against the same API version you put in `mod.json`.

## Troubleshooting

| Problem | Likely cause and fix |
|---|---|
| The Mods tab says native code is off | Turn on **Allow native mods** and confirm the DLL. |
| "refused": wrong API or ImGui version | Rebuild against the `uc_mod.h` of the game you target, and with the ImGui version it names. |
| The DLL "does not load" with no message | It is not a 64-bit DLL, `uc_mod_get_info` is missing (use `UC_MOD_DEFINE_INFO()`), or a dependent DLL is missing next to it. |
| `uc_mod_init` returns non-zero and the mod is not loaded | By design: log the reason with `api->log` before returning. |
| The UI draws garbage or crashes | You did not call `SetCurrentContext` and `SetAllocatorFunctions` at the start of every draw call, or the ImGui version differs. |
| The mod is switched off by itself | Three failures: a crash caught in a callback or callbacks over budget. Check `debug\mods.log`. |

## See also

- [Hooks](hooks.md), [Game API](game-api.md) - what to do with the API.
- [Settings and UI](settings-and-ui.md) - the script side of windows.
- [Testing and sharing](testing-and-sharing.md) - testing and packing.
- [Native DLL reference](../../native-dll.md) and the example mod `mods-examples\hello_native`.

Previous: [Hooks](hooks.md) · Next: [Testing and sharing](testing-and-sharing.md)
