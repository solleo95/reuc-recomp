// A native mod used by test_mod_native. Built several times with different definitions:
//   TEST_DLL_API_MAJOR / TEST_DLL_IMGUI  - what uc_mod_get_info claims
//   TEST_DLL_INIT_FAILS                  - uc_mod_init returns 3
#include "uc_mod.h"
#include <windows.h>

#ifndef TEST_DLL_API_MAJOR
#define TEST_DLL_API_MAJOR UC_MOD_API_MAJOR
#endif
#ifndef TEST_DLL_IMGUI
#define TEST_DLL_IMGUI UC_MOD_IMGUI_VERSION
#endif

static const UcApi* g_api;

static uint32_t read_u32(uint32_t address) {
    unsigned char b[4] = {};
    g_api->mem_read(address, b, 4);
    return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
}
static void write_u32(uint32_t address, uint32_t v) {
    const unsigned char b[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v};
    g_api->mem_write(address, b, 4);
}

static void on_step(const char*, void*) {
    switch (read_u32(0x80000308)) {
        case 1: Sleep(30); break;                                 // too slow for a small budget
        case 2: *static_cast<volatile int*>(nullptr) = 1; break;  // a crash the host must survive
        default: break;
    }
    write_u32(0x80000300, read_u32(0x80000300) + 1);
}

static void on_load(const char* payload, void*) { g_api->log(payload); }
static void hook(UcHookRegs* regs, void*) { regs->r[1] = regs->r[0] + 1; regs->f[1] = regs->f[0] * 2; }
static void post_hook(UcHookRegs* regs, void*) { regs->r[0] += 100; }      // the return value, seen after the function
static void replace_hook(UcHookRegs* regs, void*) { regs->r[0] = 4242; }   // the return value of the replacement
static void draw(void*) {
    write_u32(0x8000030C, read_u32(0x8000030C) + 1);
    write_u32(0x80000310, uint32_t(reinterpret_cast<uintptr_t>(g_api->imgui_context)));   // what a real mod passes to ImGui::SetCurrentContext
    write_u32(0x80000314, uint32_t(reinterpret_cast<uintptr_t>(g_api->imgui_alloc_user)));
}

extern "C" {
UC_MOD_EXPORT const UcModInfo* uc_mod_get_info(void) {
    static const UcModInfo info = {sizeof(UcModInfo), TEST_DLL_API_MAJOR, UC_MOD_API_MINOR, TEST_DLL_IMGUI};
    return &info;
}

UC_MOD_EXPORT int uc_mod_init(const UcApi* api) {
    g_api = api;
#ifdef TEST_DLL_INIT_FAILS
    return 3;
#endif
    g_api->log(api->mod_id);
    const char* result = nullptr;
    if (g_api->call_json("mem.read_u32", "{\"address\":2147483648}", &result) == 0) g_api->log(result);   // proves the JSON path works
    if (g_api->settings_get_json("scale", &result) == 0) g_api->log(result);
    g_api->on("on_step", on_step, nullptr);
    g_api->on("on_load", on_load, nullptr);
    g_api->set_ui(draw, nullptr);
    if (g_api->hook_pre("GameWeaponUpdate", hook, nullptr) != 0) return 4;
    if (g_api->hook_pre("NoSuchFunction", hook, nullptr) == 0) return 5;   // an unknown name must be refused
    if (g_api->hook_post("GameWeaponUpdate", post_hook, nullptr) != 0 || g_api->hook_replace("GameWeaponUpdate", replace_hook, nullptr) != 0) return 6;
    if (g_api->hook_post("NoSuchFunction", post_hook, nullptr) == 0 || g_api->hook_replace("NoSuchFunction", replace_hook, nullptr) == 0) return 7;
    return 0;
}

UC_MOD_EXPORT void uc_mod_shutdown(void) { write_u32(0x80000304, read_u32(0x80000304) + 1); }
}
