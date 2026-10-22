// Native (DLL) mods. The DLL runs with the port's full rights; what this file adds is the gate in front of it (policy, hash confirmation, version
// check) and containment of hardware exceptions inside its callbacks. See docs/modding/native-dll.md and safety.md.
#include "mod_paths.h"
#include "runtime_native.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;

namespace uc::mods {
namespace {
using Clock = std::chrono::steady_clock;
using InitFn = int (*)(const UcApi*);
using ShutdownFn = void (*)();
using InfoFn = const UcModInfo* (*)();
using EventCallback = void (*)(const char*, void*);
using DrawCallback = void (*)(void*);

// Calls into the DLL under SEH. These functions hold no C++ objects (a requirement of __try).
bool seh_init(InitFn fn, const UcApi* api, int* result, unsigned long* code) {
    __try { *result = fn(api); return true; } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_shutdown(ShutdownFn fn, unsigned long* code) {
    __try { fn(); return true; } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_event(EventCallback fn, const char* payload, void* user, unsigned long* code) {
    __try { fn(payload, user); return true; } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_draw(DrawCallback fn, void* user, unsigned long* code) {
    __try { fn(user); return true; } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return false; }
}
using HookCallbackFn = void (*)(UcHookRegs*, void*);
bool seh_hook(HookCallbackFn fn, UcHookRegs* regs, void* user, unsigned long* code) {
    __try { fn(regs, user); return true; } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// "MZ" at the start and a "PE\0\0" signature where the DOS header points: enough to keep random files away from LoadLibrary.
bool looks_like_pe(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    char dos[64];
    if (!in.read(dos, sizeof dos) || dos[0] != 'M' || dos[1] != 'Z') return false;
    int32_t offset = 0;
    std::memcpy(&offset, dos + 0x3C, sizeof offset);
    char signature[4];
    return offset > 0 && in.seekg(offset) && in.read(signature, 4) && std::memcmp(signature, "PE\0\0", 4) == 0;
}

std::string hex_code(unsigned long code) {
    char text[16];
    std::snprintf(text, sizeof text, "%08lX", code);
    return text;
}

}  // namespace

class NativeRuntime;
namespace {
thread_local NativeRuntime* t_current = nullptr;   // the runtime whose DLL is being called: the C API below has no user pointer
thread_local std::string t_result;
}  // namespace

std::string sha256_file_hex(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buffer(1 << 16);
        bool good = true;
        while (in.read(buffer.data(), std::streamsize(buffer.size())) || in.gcount() > 0) {
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), ULONG(in.gcount()), 0) != 0) { good = false; break; }
        }
        unsigned char digest[32];
        if (good && !in.bad() && BCryptFinishHash(hash, digest, sizeof digest, 0) == 0) {
            static const char* digits = "0123456789abcdef";
            for (unsigned char b : digest) { hex += digits[b >> 4]; hex += digits[b & 15]; }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return hex;
}

class NativeRuntime : public ModRuntime {
public:
    NativeRuntime(ModContext& context, NativePolicy policy) : ctx_(context), policy_(std::move(policy)) {}
    ~NativeRuntime() override { unload(); }

    std::string name() const override { return "native DLL"; }
    bool has_ui() const override { return draw_ != nullptr; }
    void set_imgui(const ImGuiHandles& handles) override {   // under the mod's lock, right before draw_ui: the DLL reads these fields when its draw callback runs
        api_.imgui_context = handles.context;
        api_.imgui_alloc = handles.alloc;
        api_.imgui_free = handles.free;
        api_.imgui_alloc_user = handles.user;
    }

    CallResult load(const fs::path& mod_root, const std::string& entry) override {
        if (!policy_.allow_native) return fail("native (DLL) mods are switched off: turn on \"Allow native mods\" in the Mods tab");
        const auto path = confine(mod_root, entry);
        if (!path || !fs::is_regular_file(*path)) return fail("cannot find " + entry);
        const std::string hash = sha256_file_hex(*path);
        if (hash.empty()) return fail("cannot read " + entry);
        if (!policy_.confirmed || !policy_.confirmed(ctx_.mod_id, hash)) return fail("the native code of this mod (sha256 " + hash + ") has not been confirmed");
        if (!looks_like_pe(*path)) return fail(entry + " is not a Windows DLL");
        // Without this Windows shows a modal "bad image" / "missing DLL" box and the game waits for a click.
        DWORD previous_mode = 0;
        SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_mode);
        module_ = LoadLibraryExW(fs::absolute(*path).c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        const DWORD load_error = GetLastError();
        SetThreadErrorMode(previous_mode, nullptr);
        if (!module_) return fail("Windows could not load " + entry + " (error " + std::to_string(load_error) + ")");
        const auto info_fn = reinterpret_cast<InfoFn>(GetProcAddress(module_, "uc_mod_get_info"));
        init_ = reinterpret_cast<InitFn>(GetProcAddress(module_, "uc_mod_init"));
        shutdown_ = reinterpret_cast<ShutdownFn>(GetProcAddress(module_, "uc_mod_shutdown"));
        if (!info_fn || !init_) { unload(); return fail("the DLL must export uc_mod_get_info (UC_MOD_DEFINE_INFO) and uc_mod_init"); }
        const UcModInfo* info = info_fn();
        if (!info || info->struct_size < sizeof(UcModInfo)) { unload(); return fail("uc_mod_get_info returned an unusable structure"); }
        if (info->api_major != UC_MOD_API_MAJOR || info->api_minor > UC_MOD_API_MINOR) {
            const std::string text = "the DLL was built for mod API " + std::to_string(info->api_major) + "." + std::to_string(info->api_minor) + ", this game offers " +
                                     std::to_string(UC_MOD_API_MAJOR) + "." + std::to_string(UC_MOD_API_MINOR);
            unload();
            return fail(text);
        }
        if (std::string(info->imgui_version, strnlen(info->imgui_version, sizeof info->imgui_version)) != policy_.imgui_version) {
            const std::string text = std::string("the DLL was built with ImGui ") + info->imgui_version + ", this game uses ImGui " + policy_.imgui_version;
            unload();
            return fail(text);
        }
        fill_api();
        int status = -1;
        unsigned long code = 0;
        t_current = this;
        const bool survived = seh_init(init_, &api_, &status, &code);
        t_current = nullptr;
        if (!survived) { unload(); return fail("uc_mod_init raised exception 0x" + hex_code(code)); }
        if (status != 0) { unload(); return fail("uc_mod_init returned " + std::to_string(status)); }
        started_ = true;
        return {};
    }

    CallResult fire(std::string_view event, const Value& payload, Budget budget) override {
        const auto at = handlers_.find(std::string(event));
        if (at == handlers_.end()) return {};
        const std::string text = payload.dump();
        const auto begin = Clock::now();
        t_current = this;
        CallResult out;
        for (const auto& handler : at->second) {
            unsigned long code = 0;
            if (!seh_event(handler.callback, text.c_str(), handler.user, &code)) { out = {false, "native code raised exception 0x" + hex_code(code), false}; break; }
        }
        t_current = nullptr;
        if (out.ok && Clock::now() - begin > budget.limit) out = {false, "native callback took longer than its time budget", true};   // cannot be interrupted, only reported
        return out;
    }

    CallResult draw_ui(Budget budget) override {
        if (!draw_) return {};
        const auto begin = Clock::now();
        unsigned long code = 0;
        t_current = this;
        const bool survived = seh_draw(draw_, draw_user_, &code);
        t_current = nullptr;
        if (!survived) return {false, "native code raised exception 0x" + hex_code(code), false};
        if (Clock::now() - begin > budget.limit) return {false, "native drawing took longer than its time budget", true};
        return {};
    }

private:
    struct Handler { EventCallback callback; void* user; };

    CallResult fail(std::string text) { return {false, std::move(text), false}; }

    void unload() {
        if (started_ && shutdown_) {
            unsigned long code = 0;
            t_current = this;
            if (!seh_shutdown(shutdown_, &code)) ctx_.log("uc_mod_shutdown raised exception 0x" + hex_code(code));
            t_current = nullptr;
        }
        started_ = false;
        handlers_.clear();
        draw_ = nullptr;
        if (module_) { FreeLibrary(module_); module_ = nullptr; }
    }

    void fill_api() {
        api_ = {};
        api_.struct_size = sizeof(UcApi);
        api_.api_major = UC_MOD_API_MAJOR;
        api_.api_minor = UC_MOD_API_MINOR;
        api_.imgui_version = policy_.imgui_version;
        id_ = ctx_.mod_id;
        api_.mod_id = id_.c_str();
        api_.log = &NativeRuntime::c_log;
        api_.mem_read = &NativeRuntime::c_mem_read;
        api_.mem_write = &NativeRuntime::c_mem_write;
        api_.get_json = &NativeRuntime::c_get_json;
        api_.set_json = &NativeRuntime::c_set_json;
        api_.call_json = &NativeRuntime::c_call_json;
        api_.settings_get_json = &NativeRuntime::c_settings_get;
        api_.settings_set_json = &NativeRuntime::c_settings_set;
        api_.on = &NativeRuntime::c_on;
        api_.set_ui = &NativeRuntime::c_set_ui;
        api_.hook_pre = &NativeRuntime::c_hook_pre;
    }

    static int c_hook_pre(const char* name, HookCallbackFn callback, void* user) {
        auto* self = current();
        if (!self || !name || !callback || !self->ctx_.hooks) return 1;
        const bool added = self->ctx_.hooks->add_pre(self, name, [self, callback, user](HookRegs& regs, Budget budget) -> CallResult {
            UcHookRegs native{};
            for (int i = 0; i < 8; ++i) { native.r[i] = regs.r[i]; native.f[i] = regs.f[i]; }
            const auto begin = Clock::now();
            NativeRuntime* previous = t_current;
            t_current = self;
            unsigned long code = 0;
            const bool survived = seh_hook(callback, &native, user, &code);
            t_current = previous;
            if (!survived) return {false, "native hook raised exception 0x" + hex_code(code), false};
            for (int i = 0; i < 8; ++i) { regs.r[i] = native.r[i]; regs.f[i] = native.f[i]; }
            if (Clock::now() - begin > budget.limit) return {false, "native hook took longer than its time budget", true};
            return {};
        });
        return added ? 0 : 1;
    }

    // ---- the C API handed to the DLL ----
    static NativeRuntime* current() { return t_current; }
    static int report(const std::exception& e, const char** result) { t_result = e.what(); *result = t_result.c_str(); return 1; }

    static void c_log(const char* line) { if (auto* self = current()) self->ctx_.log(line ? line : ""); }
    static int c_mem_read(uint32_t address, void* destination, uint32_t length) {
        auto* self = current();
        return self && self->ctx_.memory && destination && self->ctx_.memory->read_bytes(address, destination, length) ? 1 : 0;
    }
    static int c_mem_write(uint32_t address, const void* source, uint32_t length) {
        auto* self = current();
        return self && self->ctx_.memory && source && self->ctx_.memory->write_bytes(address, source, length) ? 1 : 0;
    }
    static Value parse_json(const char* text) {
        if (!text || !*text) return Value::object();
        Value value = Value::parse(text, nullptr, false);
        if (value.is_discarded()) throw ApiError("the JSON text could not be parsed");
        return value;
    }
    static int c_get_json(const char* path, const char* args_json, const char** result) {
        auto* self = current();
        try {
            const Member* member = self && path ? self->ctx_.api.find(path) : nullptr;
            if (!member || !member->get) throw ApiError(std::string("no readable API member '") + (path ? path : "") + "'");
            t_result = member->get(parse_json(args_json)).dump();
            *result = t_result.c_str();
            return 0;
        } catch (const std::exception& e) { return report(e, result); }
    }
    static int c_set_json(const char* path, const char* args_json, const char* value_json, const char** result) {
        auto* self = current();
        try {
            const Member* member = self && path ? self->ctx_.api.find(path) : nullptr;
            if (!member || !member->set) throw ApiError(std::string("no writable API member '") + (path ? path : "") + "'");
            member->set(parse_json(args_json), parse_json(value_json ? value_json : "null"));
            t_result.clear();
            *result = t_result.c_str();
            return 0;
        } catch (const std::exception& e) { return report(e, result); }
    }
    static int c_call_json(const char* path, const char* args_json, const char** result) {
        auto* self = current();
        try {
            const Member* member = self && path ? self->ctx_.api.find(path) : nullptr;
            if (!member || !member->call) throw ApiError(std::string("no API function '") + (path ? path : "") + "'");
            t_result = member->call(parse_json(args_json)).dump();
            *result = t_result.c_str();
            return 0;
        } catch (const std::exception& e) { return report(e, result); }
    }
    static int c_settings_get(const char* key, const char** result) {
        auto* self = current();
        if (!self || !key) { t_result = "no settings key"; *result = t_result.c_str(); return 1; }
        t_result = self->ctx_.settings.get(key).dump();
        *result = t_result.c_str();
        return 0;
    }
    static int c_settings_set(const char* key, const char* value_json) {
        auto* self = current();
        if (!self || !key) return 1;
        const Value value = Value::parse(value_json ? value_json : "null", nullptr, false);
        return !value.is_discarded() && self->ctx_.settings.set(key, value) ? 0 : 1;
    }
    static void c_on(const char* event, EventCallback callback, void* user) {
        auto* self = current();
        if (self && event && callback && self->handlers_[event].size() < 256) self->handlers_[event].push_back({callback, user});
    }
    static void c_set_ui(DrawCallback draw, void* user) {
        if (auto* self = current()) { self->draw_ = draw; self->draw_user_ = user; }
    }

    ModContext& ctx_;
    NativePolicy policy_;
    HMODULE module_ = nullptr;
    InitFn init_ = nullptr;
    ShutdownFn shutdown_ = nullptr;
    bool started_ = false;
    UcApi api_{};
    std::string id_;
    std::map<std::string, std::vector<Handler>> handlers_;
    DrawCallback draw_ = nullptr;
    void* draw_user_ = nullptr;
};

std::unique_ptr<ModRuntime> make_native_runtime(ModContext& context, NativePolicy policy) { return std::make_unique<NativeRuntime>(context, std::move(policy)); }

}  // namespace uc::mods
