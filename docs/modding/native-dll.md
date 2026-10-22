# Native (DLL) mods

A DLL mod runs inside the game process with full rights: it can do anything the player's account can. Use it for work that scripts are too slow or too limited for. The port gates it: the "Allow native mods" switch in the Mods tab (default off) and a per-DLL confirmation tied to the file's SHA-256 (see safety.md).

Set `"native": "native/mod.dll"` in `mod.json`. A mod may have both a script and a DLL.

## Writing one

Build a 64-bit DLL against [`modding/uc_mod.h`](../../modding/uc_mod.h) (C or C++):

```c
#include "uc_mod.h"

UC_MOD_DEFINE_INFO()                       /* exports uc_mod_get_info: API and ImGui version of this build */

static const UcApi* g_api;

static void on_step(const char* payload_json, void* user) {
    uint8_t bytes[4] = {0, 0, 0, 15};      /* big-endian 15 */
    g_api->mem_write(0x80000000, bytes, 4);
}

UC_MOD_EXPORT int uc_mod_init(const UcApi* api) {
    g_api = api;
    api->log("started");
    api->on("on_step", on_step, NULL);
    return 0;                              /* non-zero: the mod is not loaded */
}

UC_MOD_EXPORT void uc_mod_shutdown(void) {}
```

The port refuses the DLL when `uc_mod_get_info` is missing or reports another API major version, a newer minor version than the game offers, or another Dear ImGui version than `UC_MOD_IMGUI_VERSION`.

## The API table

`UcApi` (see the header for exact types):

| Member | Use |
|---|---|
| `log` | Write a line to the mod log. |
| `mem_read`, `mem_write` | Guest memory as raw big-endian bytes; return 1 on success. |
| `get_json`, `set_json`, `call_json` | The whole [game API](api/README.md) through JSON text: `get_json("player.magazine", "{\"slot\":1}", &out)`. Return 0 on success; `out` is then the JSON value, otherwise the error text. |
| `settings_get_json`, `settings_set_json` | The mod's declared settings. |
| `on(event, callback, user)` | Events (`on_step`, `on_load`, ...). The payload is JSON text. |
| `hook_pre(name, callback, user)` | Run `callback(regs, user)` before a hookable game function; changes to `regs->r[]` (r3..r10) and `regs->f[]` (f1..f8) go back into the game. |
| `set_ui(draw, user)` | A function called every frame the mod's tab is visible, inside an ImGui window. |
| `imgui_context`, `imgui_alloc`, `imgui_free`, `imgui_alloc_user` | For drawing with Dear ImGui. The port fills them in right before each call of your draw function; they are empty when the DLL loads. |

Strings the port returns stay valid until your next API call. Call the API only from `uc_mod_init` and from your callbacks, on the thread the port calls you on (the game thread for events and hooks, the menu thread for drawing).

## Drawing with ImGui

Build your DLL with its own copy of Dear ImGui **of exactly the version in `UC_MOD_IMGUI_VERSION`**, then at the start of every call of your draw function (not in `uc_mod_init`: the context does not exist yet):

```cpp
ImGui::SetCurrentContext(static_cast<ImGuiContext*>(api->imgui_context));
ImGui::SetAllocatorFunctions(api->imgui_alloc, api->imgui_free, api->imgui_alloc_user);
```

and draw normal widgets in the function registered with `set_ui`.

## What the port does for you

- A hardware exception (access violation and similar) inside a callback is caught and counts as a failure of the mod. Three failures switch the mod off. The DLL stays loaded (the port cannot make a corrupt DLL safe).
- Callbacks are timed after they return; one that runs longer than its budget (1 ms for steps, 4 ms for drawing) counts as a failure. A native callback cannot be interrupted.
- Windows' own "bad image" error boxes are suppressed; a file that is not a DLL is refused with a message.
