/* Public C header for native (DLL) mods of Umbrella Chronicles PC. Build your DLL against this header and export:
 *
 *     UC_MOD_DEFINE_INFO()                      // once: tells the port which API and ImGui version the DLL was built with
 *     UC_MOD_EXPORT int  uc_mod_init(const UcApi* api);   // return 0 when the mod started
 *     UC_MOD_EXPORT void uc_mod_shutdown(void);           // optional but recommended
 *
 * Everything in UcApi must be called from inside uc_mod_init or from one of the callbacks you registered, on the thread the port
 * calls you on. Strings returned through "const char**" parameters belong to the port and stay valid until your next API call.
 * See docs/modding/native-dll.md.
 */
#ifndef UC_MOD_H
#define UC_MOD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UC_MOD_API_MAJOR 1
#define UC_MOD_API_MINOR 1
/* The Dear ImGui version the port is built with. A DLL that draws with ImGui must be built with exactly this version. */
#define UC_MOD_IMGUI_VERSION "1.92.9b"

#ifdef _WIN32
#define UC_MOD_EXPORT __declspec(dllexport)
#else
#define UC_MOD_EXPORT
#endif

typedef struct UcModInfo {
    uint32_t struct_size;
    uint32_t api_major;
    uint32_t api_minor;
    char imgui_version[16];
} UcModInfo;

#define UC_MOD_DEFINE_INFO() \
    UC_MOD_EXPORT const UcModInfo* uc_mod_get_info(void) { \
        static const UcModInfo info = {sizeof(UcModInfo), UC_MOD_API_MAJOR, UC_MOD_API_MINOR, UC_MOD_IMGUI_VERSION}; \
        return &info; \
    }

/* r3..r10 and f1..f8 of the game's PowerPC state at the entry of a hooked function. */
typedef struct UcHookRegs {
    uint32_t r[8];
    double f[8];
} UcHookRegs;

typedef struct UcApi {
    uint32_t struct_size;
    uint32_t api_major;
    uint32_t api_minor;
    uint32_t reserved;

    /* Dear ImGui: at the start of every call of your draw function (set_ui), call ImGui::SetCurrentContext(imgui_context) and
     * ImGui::SetAllocatorFunctions(imgui_alloc, imgui_free, imgui_alloc_user), with the same ImGui version as imgui_version.
     * The port fills these three fields right before each draw call; they are null while uc_mod_init runs. */
    const char* imgui_version;
    void* imgui_context;
    void* (*imgui_alloc)(size_t size, void* user);
    void (*imgui_free)(void* pointer, void* user);
    void* imgui_alloc_user;

    const char* mod_id;
    void (*log)(const char* line);

    /* Guest memory, big-endian bytes exactly as the game stores them. Return 1 on success, 0 when the range is not inside MEM1/MEM2. */
    int (*mem_read)(uint32_t address, void* destination, uint32_t length);
    int (*mem_write)(uint32_t address, const void* source, uint32_t length);

    /* The game API of docs/modding/api/. path is "domain.member" ("player.magazine"), args_json an object with the argument names
     * (and "slot"/"name"/"id" for scoped domains). They return 0 on success; *result is then the JSON value (get, call) or ""
     * (set). On failure they return non-zero and *result is the error text. */
    int (*get_json)(const char* path, const char* args_json, const char** result);
    int (*set_json)(const char* path, const char* args_json, const char* value_json, const char** result);
    int (*call_json)(const char* path, const char* args_json, const char** result);

    /* The mod's declared settings (mod.json). settings_get_json gives the value as JSON text. Return 0 on success. */
    int (*settings_get_json)(const char* key, const char** result);
    int (*settings_set_json)(const char* key, const char* value_json);

    /* Events of the game API ("on_step", "on_load", ...). The payload is JSON text. */
    void (*on)(const char* event, void (*callback)(const char* payload_json, void* user), void* user);
    /* Called every frame the mod's settings window (gear button in the Mods tab of the Insert menu) is open, inside an ImGui window. */
    void (*set_ui)(void (*draw)(void* user), void* user);

    /* Runs callback before the hookable game function `name` (docs/modding/api/README.md lists them). regs holds the argument registers;
     * changes written to it go back into the game. Returns 0, or non-zero when `name` is not hookable. */
    int (*hook_pre)(const char* name, void (*callback)(UcHookRegs* regs, void* user), void* user);
    /* Since API 1.1. hook_replace runs callback instead of the game function: r[0] (r3) and f[0] (f1) of regs are its return value.
     * hook_post runs callback after the function returned (r[0], f[0] hold the return value and may be changed). Same return values as hook_pre. */
    int (*hook_replace)(const char* name, void (*callback)(UcHookRegs* regs, void* user), void* user);
    int (*hook_post)(const char* name, void (*callback)(UcHookRegs* regs, void* user), void* user);
} UcApi;

#ifdef __cplusplus
}
#endif

#endif
