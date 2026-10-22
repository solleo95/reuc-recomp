// Lua 5.4 runtime for mods. Lua is compiled as C++ (see modding/CMakeLists.txt), so its errors are C++ exceptions and the destructors
// of our frames run when a script error unwinds through them. Never catch(...) in code that calls the Lua API: that would swallow Lua's own exceptions.
#include "mod_paths.h"
#include "runtime.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <map>

namespace fs = std::filesystem;

namespace uc::mods {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kMemoryCap = 64u << 20;
constexpr int kMaxDepth = 16;
constexpr auto kLoadBudget = std::chrono::seconds(2);

class LuaRuntime;
LuaRuntime*& self_of(lua_State* L) { return *static_cast<LuaRuntime**>(lua_getextraspace(L)); }

struct DomainInfo {
    std::string name;
    std::string scope;   // empty for a global domain
    std::map<std::string, const Member*> properties;
};

Value to_value(lua_State* L, int index, int depth = 0);

bool is_array(lua_State* L, int index, size_t& count) {
    count = 0;
    size_t integers = 0;
    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        ++count;
        if (lua_isinteger(L, -2)) {
            const lua_Integer k = lua_tointeger(L, -2);
            if (k >= 1 && size_t(k) <= 1000000) ++integers;
        }
        lua_pop(L, 1);
    }
    return integers == count;   // every key is a positive integer: treated as a list when the keys are 1..n
}

Value to_value(lua_State* L, int index, int depth) {
    if (depth > kMaxDepth) throw ApiError("value is nested too deeply");
    switch (lua_type(L, index)) {
        case LUA_TNIL: return Value(nullptr);
        case LUA_TBOOLEAN: return Value(lua_toboolean(L, index) != 0);
        case LUA_TNUMBER:
            if (lua_isinteger(L, index)) return Value(static_cast<long long>(lua_tointeger(L, index)));
            if (!std::isfinite(lua_tonumber(L, index))) throw ApiError("numbers passed to the game API must be finite");
            return Value(static_cast<double>(lua_tonumber(L, index)));
        case LUA_TSTRING: { size_t n; const char* s = lua_tolstring(L, index, &n); return Value(std::string(s, n)); }
        case LUA_TTABLE: {
            size_t count;
            Value result;
            if (is_array(L, index, count)) {
                result = Value::array();
                for (size_t i = 1; i <= count; ++i) {
                    lua_geti(L, index, lua_Integer(i));
                    if (lua_isnil(L, -1)) { lua_pop(L, 1); throw ApiError("lists passed to the game API must not have holes"); }
                    result.push_back(to_value(L, -1, depth + 1));
                    lua_pop(L, 1);
                }
                return result;
            }
            result = Value::object();
            const int table = lua_absindex(L, index);
            lua_pushnil(L);
            while (lua_next(L, table)) {
                if (lua_type(L, -2) != LUA_TSTRING) { lua_pop(L, 2); throw ApiError("tables passed to the game API need string keys"); }
                size_t n; const char* key = lua_tolstring(L, -2, &n);
                result[std::string(key, n)] = to_value(L, -1, depth + 1);
                lua_pop(L, 1);
            }
            return result;
        }
        default: throw ApiError(std::string("a ") + luaL_typename(L, index) + " cannot be passed to the game API");
    }
}

void push_value(lua_State* L, const Value& value, int depth = 0) {
    if (depth > kMaxDepth) throw ApiError("value is nested too deeply");
    luaL_checkstack(L, 4, "too many nested values");
    switch (value.type()) {
        case Value::value_t::null: lua_pushnil(L); break;
        case Value::value_t::boolean: lua_pushboolean(L, value.get<bool>()); break;
        case Value::value_t::number_integer: case Value::value_t::number_unsigned: lua_pushinteger(L, static_cast<lua_Integer>(value.get<long long>())); break;
        case Value::value_t::number_float: lua_pushnumber(L, value.get<double>()); break;
        case Value::value_t::string: { const auto& s = value.get_ref<const std::string&>(); lua_pushlstring(L, s.data(), s.size()); break; }
        case Value::value_t::array:
            lua_createtable(L, int(value.size()), 0);
            for (size_t i = 0; i < value.size(); ++i) { push_value(L, value[i], depth + 1); lua_rawseti(L, -2, lua_Integer(i + 1)); }
            break;
        case Value::value_t::object:
            lua_createtable(L, 0, int(value.size()));
            for (const auto& [key, item] : value.items()) { push_value(L, item, depth + 1); lua_setfield(L, -2, key.c_str()); }
            break;
        default: lua_pushnil(L);
    }
}

// Runs f; an exception becomes a Lua error raised after f's locals are gone.
template <class F> int guarded(lua_State* L, F&& f) {
    std::string error;
    int results = 0;
    try { results = f(); } catch (const std::exception& e) { error = e.what(); }
    if (error.empty()) return results;
    lua_pushlstring(L, error.data(), error.size());
    std::string().swap(error);
    return lua_error(L);
}

class LuaRuntime : public ModRuntime {
public:
    explicit LuaRuntime(ModContext& context) : ctx_(context) {
        L_ = lua_newstate(&LuaRuntime::allocate, this);
        if (!L_) throw std::runtime_error("cannot create a Lua state");
        self_of(L_) = this;
        lua_sethook(L_, &LuaRuntime::count_hook, LUA_MASKCOUNT, 1000);
        deadline_ = Clock::time_point::max();
        open_libraries();
        install_context_globals();
        install_uc();
    }
    ~LuaRuntime() override { lua_close(L_); }

    std::string name() const override { return "Lua 5.4"; }
    bool has_ui() const override { return draw_ref_ != LUA_NOREF; }

    CallResult load(const fs::path& mod_root, const std::string& entry) override {
        root_ = mod_root;
        std::string error;
        if (!run_file(entry, error)) return {false, error, over_budget_};
        return {};
    }

    CallResult fire(std::string_view event, const Value& payload, Budget budget) override {
        const auto at = handlers_.find(std::string(event));
        if (at == handlers_.end() || at->second.empty()) return {};
        const auto refs = at->second;   // a handler may register more handlers
        for (const int ref : refs) {
            lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
            try { push_value(L_, payload); } catch (const std::exception& e) { lua_pop(L_, 1); return {false, e.what(), false}; }
            if (const auto result = protected_call(1, budget.limit); !result.ok) return result;
        }
        return {};
    }

    CallResult draw_ui(Budget budget) override {
        if (draw_ref_ == LUA_NOREF) return {};
        lua_rawgeti(L_, LUA_REGISTRYINDEX, draw_ref_);
        in_ui_ = true;
        const auto result = protected_call(0, budget.limit);
        in_ui_ = false;
        return result;
    }

private:
    static void* allocate(void* ud, void* ptr, size_t old_size, size_t new_size) {
        auto* self = static_cast<LuaRuntime*>(ud);
        if (new_size == 0) {
            if (ptr) self->used_ -= old_size;
            std::free(ptr);
            return nullptr;
        }
        const size_t before = ptr ? old_size : 0;   // for a new block old_size is a type tag
        if (new_size > before && self->used_ + (new_size - before) > kMemoryCap) return nullptr;
        void* block = std::realloc(ptr, new_size);
        if (block) self->used_ = self->used_ - before + new_size;
        return block;
    }

    static void count_hook(lua_State* L, lua_Debug*) { check_budget(L); }

public:
    // Called by the instruction-count hook and by the patched pattern matcher (a single string.find can run for minutes without executing a Lua instruction).
    static void check_budget(lua_State* L) {
        auto* self = self_of(L);
        if (Clock::now() > self->deadline_) {
            self->over_budget_ = true;   // sticky for the whole call: the error is raised again at every tick, and pcall/xpcall/coroutine.resume re-raise it
            luaL_error(L, "script exceeded its time budget");
        }
    }

private:
    // pcall/xpcall as in lbaselib.c, except that the time-budget error cannot be swallowed (a script could otherwise loop forever inside pcall).
    static int finish_pcall(lua_State* L, int status, lua_KContext extra) {
        if (status != LUA_OK && status != LUA_YIELD) {
            if (me(L)->over_budget_) luaL_error(L, "script exceeded its time budget");
            lua_pushboolean(L, 0);
            lua_pushvalue(L, -2);
            return 2;
        }
        return lua_gettop(L) - static_cast<int>(extra);
    }
    static int l_pcall(lua_State* L) {
        luaL_checkany(L, 1);
        lua_pushboolean(L, 1);
        lua_insert(L, 1);
        return finish_pcall(L, lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, &LuaRuntime::finish_pcall), 0);
    }
    static int l_xpcall(lua_State* L) {
        const int n = lua_gettop(L);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_pushboolean(L, 1);
        lua_pushvalue(L, 1);
        lua_rotate(L, 3, 2);
        return finish_pcall(L, lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, &LuaRuntime::finish_pcall), 2);
    }

    // The function and its nargs arguments are on the stack. Pops them all.
    CallResult protected_call(int nargs, std::chrono::microseconds limit) {
        over_budget_ = false;
        deadline_ = Clock::now() + limit;
        const int status = lua_pcall(L_, nargs, 0, 0);
        deadline_ = Clock::time_point::max();
        if (status == LUA_OK) return {};
        CallResult result{false, error_text(), over_budget_};
        lua_pop(L_, 1);
        return result;
    }

    std::string error_text() {
        if (lua_type(L_, -1) == LUA_TSTRING) return lua_tostring(L_, -1);
        return std::string("(error object is a ") + luaL_typename(L_, -1) + " value)";
    }

    bool run_file(const std::string& relative, std::string& error) {
        const auto path = confine(root_, relative);
        const auto text = path ? read_file(*path) : std::nullopt;
        if (!text) { error = "cannot read " + relative; return false; }
        const std::string chunk = "@" + relative;
        if (luaL_loadbufferx(L_, text->data(), text->size(), chunk.c_str(), "t") != LUA_OK) {
            error = error_text();
            lua_pop(L_, 1);
            return false;
        }
        const auto result = protected_call(0, kLoadBudget);
        error = result.error;
        return result.ok;
    }

    void open_libraries() {
        for (const auto& [name, open] : {std::pair<const char*, lua_CFunction>{LUA_GNAME, luaopen_base}, {LUA_STRLIBNAME, luaopen_string},
                                         {LUA_TABLIBNAME, luaopen_table}, {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}}) {
            luaL_requiref(L_, name, open, 1);
            lua_pop(L_, 1);
        }
        lua_pushcfunction(L_, &LuaRuntime::l_pcall); lua_setglobal(L_, "pcall");
        lua_pushcfunction(L_, &LuaRuntime::l_xpcall); lua_setglobal(L_, "xpcall");
        for (const char* removed : {"dofile", "loadfile", "load", "loadstring", "print", "collectgarbage"}) { lua_pushnil(L_); lua_setglobal(L_, removed); }
        lua_getglobal(L_, "string"); lua_pushnil(L_); lua_setfield(L_, -2, "dump"); lua_pop(L_, 1);
        lua_pushcfunction(L_, &LuaRuntime::l_collectgarbage); lua_setglobal(L_, "collectgarbage");
        lua_pushcfunction(L_, &LuaRuntime::l_print); lua_setglobal(L_, "print");
        lua_pushcfunction(L_, &LuaRuntime::l_require); lua_setglobal(L_, "require");
        lua_newtable(L_); lua_setfield(L_, LUA_REGISTRYINDEX, "uc_loaded");
    }

    void install_context_globals();
    void install_uc();

    // ---- C functions ----
    static LuaRuntime* me(lua_State* L) { return self_of(L); }

    static int l_collectgarbage(lua_State* L) {
        const char* option = luaL_optstring(L, 1, "count");
        if (std::string_view(option) != "count") return luaL_error(L, "collectgarbage('%s') is not available to mods", option);
        lua_pushnumber(L, double(me(L)->used_) / 1024.0);
        return 1;
    }

    static std::string join_arguments(lua_State* L) {
        std::string line;
        for (int i = 1, n = lua_gettop(L); i <= n; ++i) {
            size_t length;
            const char* s = luaL_tolstring(L, i, &length);
            if (i > 1) line += '\t';
            line.append(s, length);
            lua_pop(L, 1);
        }
        return line;
    }
    static int l_print(lua_State* L) { me(L)->ctx_.log(join_arguments(L)); return 0; }
    static int l_log_write(lua_State* L) { me(L)->ctx_.log(join_arguments(L)); return 0; }

    static int l_require(lua_State* L) {
        size_t length;
        const char* raw = luaL_checklstring(L, 1, &length);
        const std::string name(raw, length);
        std::string relative;
        const bool good = !name.empty() && name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") == std::string::npos &&
                          name.front() != '.' && name.back() != '.' && name.find("..") == std::string::npos;
        if (!good) return luaL_error(L, "bad module name '%s' (use letters, digits, '_', '-' and '.' between folder names)", name.c_str());
        relative = "scripts/";
        for (char c : name) relative += c == '.' ? '/' : c;
        relative += ".lua";
        lua_getfield(L, LUA_REGISTRYINDEX, "uc_loaded");
        lua_getfield(L, -1, name.c_str());
        if (!lua_isnil(L, -1)) return 1;
        lua_pop(L, 1);
        LuaRuntime* self = me(L);
        const auto path = confine(self->root_, relative);
        const auto text = path ? read_file(*path) : std::nullopt;
        if (!text) return luaL_error(L, "module '%s' not found (looked for %s)", name.c_str(), relative.c_str());
        const std::string chunk = "@" + relative;
        if (luaL_loadbufferx(L, text->data(), text->size(), chunk.c_str(), "t") != LUA_OK) return lua_error(L);
        lua_pushvalue(L, 1);
        lua_call(L, 1, 1);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushboolean(L, 1); }
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, name.c_str());   // uc_loaded[name] = result
        return 1;
    }

    static int l_settings_get(lua_State* L) {
        return guarded(L, [&] { push_value(L, me(L)->ctx_.settings.get(luaL_checkstring(L, 1))); return 1; });
    }
    static int l_settings_set(lua_State* L) {
        return guarded(L, [&] {
            const std::string key = luaL_checkstring(L, 1);
            lua_pushboolean(L, me(L)->ctx_.settings.set(key, to_value(L, 2)));
            return 1;
        });
    }

    static int l_on(lua_State* L) {
        const std::string event = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_pushvalue(L, 2);
        auto& list = me(L)->handlers_[event];
        if (list.size() >= 256) return luaL_error(L, "too many handlers for '%s'", event.c_str());
        list.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
        return 0;
    }

    // hook.pre(name, fn): fn gets a table with r3..r10 (integers) and f1..f8 (numbers) and may change them.
    CallResult call_hook(int ref, HookRegs& regs, Budget budget) {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
        lua_createtable(L_, 0, 16);
        for (int i = 0; i < 8; ++i) {
            lua_pushinteger(L_, lua_Integer(regs.r[i])); lua_setfield(L_, -2, ("r" + std::to_string(3 + i)).c_str());
            lua_pushnumber(L_, regs.f[i]); lua_setfield(L_, -2, ("f" + std::to_string(1 + i)).c_str());
        }
        lua_pushvalue(L_, -1);
        const int table = luaL_ref(L_, LUA_REGISTRYINDEX);   // keeps the table for reading back; pops the copy
        const CallResult result = protected_call(1, budget.limit);
        if (result.ok) {
            lua_rawgeti(L_, LUA_REGISTRYINDEX, table);
            for (int i = 0; i < 8; ++i) {
                lua_getfield(L_, -1, ("r" + std::to_string(3 + i)).c_str());
                if (lua_isinteger(L_, -1) || lua_isnumber(L_, -1)) regs.r[i] = uint32_t(lua_tointeger(L_, -1));
                lua_pop(L_, 1);
                lua_getfield(L_, -1, ("f" + std::to_string(1 + i)).c_str());
                if (lua_isnumber(L_, -1)) regs.f[i] = lua_tonumber(L_, -1);
                lua_pop(L_, 1);
            }
            lua_pop(L_, 1);
        }
        luaL_unref(L_, LUA_REGISTRYINDEX, table);
        return result;
    }
    static int l_hook_pre(lua_State* L) {
        const std::string name = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        LuaRuntime* self = me(L);
        if (!self->ctx_.hooks) return luaL_error(L, "hooks are not available here (they need \"touches_gameplay\": true in mod.json)");
        lua_pushvalue(L, 2);
        const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        if (!self->ctx_.hooks->add_pre(self, name, [self, ref](HookRegs& regs, Budget budget) { return self->call_hook(ref, regs, budget); })) {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return luaL_error(L, "'%s' is not a hookable function (see hook.list())", name.c_str());
        }
        return 0;
    }
    static int l_hook_list(lua_State* L) {
        const auto names = me(L)->ctx_.hooks ? me(L)->ctx_.hooks->names() : std::vector<std::string>();
        lua_createtable(L, int(names.size()), 0);
        for (size_t i = 0; i < names.size(); ++i) { lua_pushlstring(L, names[i].data(), names[i].size()); lua_rawseti(L, -2, lua_Integer(i + 1)); }
        return 1;
    }
    static int l_hook_unavailable(lua_State* L) { return luaL_error(L, "hook.post and hook.replace are not available in this version (see the API reference)"); }

    static int l_ui_draw(lua_State* L) {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        LuaRuntime* self = me(L);
        if (self->draw_ref_ != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, self->draw_ref_);
        lua_pushvalue(L, 1);
        self->draw_ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
        return 0;
    }

    // ui.* : the widget call is forwarded while the tab is drawn; the function arguments are read before any C++ object is created.
    static UiApi& ui_or_error(lua_State* L) {
        LuaRuntime* self = me(L);
        if (!self->in_ui_ || !self->ctx_.ui) luaL_error(L, "ui functions can only be used inside the function given to ui.draw");
        return *self->ctx_.ui;
    }
    static int l_ui_text(lua_State* L) { return guarded(L, [&] { ui_or_error(L).text(luaL_checkstring(L, 1)); return 0; }); }
    static int l_ui_button(lua_State* L) { return guarded(L, [&] { lua_pushboolean(L, ui_or_error(L).button(luaL_checkstring(L, 1))); return 1; }); }
    static int l_ui_checkbox(lua_State* L) { return guarded(L, [&] { luaL_checktype(L, 2, LUA_TBOOLEAN); lua_pushboolean(L, ui_or_error(L).checkbox(luaL_checkstring(L, 1), lua_toboolean(L, 2) != 0)); return 1; }); }
    static int l_ui_slider_int(lua_State* L) {
        return guarded(L, [&] { lua_pushinteger(L, ui_or_error(L).slider_int(luaL_checkstring(L, 1), int(luaL_checkinteger(L, 2)), int(luaL_checkinteger(L, 3)), int(luaL_checkinteger(L, 4)))); return 1; });
    }
    static int l_ui_slider_float(lua_State* L) {
        return guarded(L, [&] { lua_pushnumber(L, ui_or_error(L).slider_float(luaL_checkstring(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4))); return 1; });
    }
    static int l_ui_combo(lua_State* L) {
        return guarded(L, [&] {
            luaL_checktype(L, 3, LUA_TTABLE);
            std::vector<std::string> items;
            for (lua_Integer i = 1, n = luaL_len(L, 3); i <= n; ++i) { lua_geti(L, 3, i); items.push_back(luaL_tolstring(L, -1, nullptr)); lua_pop(L, 2); }
            lua_pushinteger(L, ui_or_error(L).combo(luaL_checkstring(L, 1), int(luaL_checkinteger(L, 2)), items));
            return 1;
        });
    }
    static int l_ui_input_text(lua_State* L) {
        return guarded(L, [&] { const auto result = ui_or_error(L).input_text(luaL_checkstring(L, 1), luaL_checkstring(L, 2)); lua_pushlstring(L, result.data(), result.size()); return 1; });
    }
    static int l_ui_separator(lua_State* L) { return guarded(L, [&] { ui_or_error(L).separator(); return 0; }); }
    static int l_ui_same_line(lua_State* L) { return guarded(L, [&] { ui_or_error(L).same_line(); return 0; }); }
    static int l_ui_header(lua_State* L) { return guarded(L, [&] { lua_pushboolean(L, ui_or_error(L).collapsing_header(luaL_checkstring(L, 1))); return 1; }); }
    static int l_ui_tooltip(lua_State* L) { return guarded(L, [&] { ui_or_error(L).tooltip(luaL_checkstring(L, 1)); return 0; }); }
    static int l_ui_color_edit(lua_State* L) {
        return guarded(L, [&] {
            const std::string label = luaL_checkstring(L, 1);
            luaL_checktype(L, 2, LUA_TTABLE);
            std::array<float, 4> rgba{};
            for (int i = 0; i < 4; ++i) { lua_geti(L, 2, i + 1); rgba[size_t(i)] = float(luaL_checknumber(L, -1)); lua_pop(L, 1); }
            rgba = ui_or_error(L).color_edit(label, rgba);
            lua_createtable(L, 4, 0);
            for (int i = 0; i < 4; ++i) { lua_pushnumber(L, rgba[size_t(i)]); lua_rawseti(L, -2, i + 1); }
            return 1;
        });
    }

    // uc.<domain>.<function>(...): positional arguments become the named object the registry expects.
    static int l_api_call(lua_State* L) {
        const auto* member = static_cast<const Member*>(lua_touserdata(L, lua_upvalueindex(1)));
        return guarded(L, [&] {
            Value args = Value::object();
            const int count = lua_gettop(L);
            if (size_t(count) > member->arg_names.size()) throw ApiError(member->path + " takes " + std::to_string(member->arg_names.size()) + " argument(s)");
            for (int i = 1; i <= count; ++i) args[member->arg_names[size_t(i - 1)]] = to_value(L, i);
            push_value(L, member->call(args));
            return 1;
        });
    }

    // __index / __newindex of a domain table (global domains) or of a handle (scoped domains; the scope value is handle[1]).
    static int l_prop_index(lua_State* L) {
        const auto* info = static_cast<const DomainInfo*>(lua_touserdata(L, lua_upvalueindex(1)));
        return guarded(L, [&] {
            const std::string key = luaL_checkstring(L, 2);
            const auto at = info->properties.find(key);
            if (at == info->properties.end() || !at->second->get) throw ApiError("uc." + info->name + " has no readable member '" + key + "' (it may not be implemented yet: see the API reference)");
            Value args = Value::object();
            if (!info->scope.empty()) { lua_rawgeti(L, 1, 1); args[info->scope] = to_value(L, -1); lua_pop(L, 1); }
            push_value(L, at->second->get(args));
            return 1;
        });
    }
    static int l_prop_newindex(lua_State* L) {
        const auto* info = static_cast<const DomainInfo*>(lua_touserdata(L, lua_upvalueindex(1)));
        return guarded(L, [&] {
            const std::string key = luaL_checkstring(L, 2);
            const auto at = info->properties.find(key);
            if (at == info->properties.end() || !at->second->set) throw ApiError("uc." + info->name + " has no writable member '" + key + "'");
            Value args = Value::object();
            if (!info->scope.empty()) { lua_rawgeti(L, 1, 1); args[info->scope] = to_value(L, -1); lua_pop(L, 1); }
            at->second->set(args, to_value(L, 3));
            return 0;
        });
    }
    // uc.player(1): a handle table whose metatable is the domain's.
    static int l_scoped_handle(lua_State* L) {
        const auto* info = static_cast<const DomainInfo*>(lua_touserdata(L, lua_upvalueindex(1)));
        return guarded(L, [&] {
            if (lua_gettop(L) != 1) throw ApiError("uc." + info->name + "() takes one argument: the " + info->scope);
            const Value scope = to_value(L, 1);   // validated by the members; a wrong type fails there
            lua_createtable(L, 1, 0);
            push_value(L, scope);
            lua_rawseti(L, -2, 1);
            luaL_setmetatable(L, ("uc_" + info->name).c_str());
            return 1;
        });
    }

    ModContext& ctx_;
    lua_State* L_ = nullptr;
    size_t used_ = 0;
    Clock::time_point deadline_;
    bool over_budget_ = false;
    bool in_ui_ = false;
    int draw_ref_ = LUA_NOREF;
    fs::path root_;
    std::map<std::string, std::vector<int>> handlers_;
    std::deque<DomainInfo> domains_;   // stable addresses: C closures point at them
};

void LuaRuntime::install_context_globals() {
    lua_newtable(L_);
    lua_pushcfunction(L_, &l_log_write); lua_setfield(L_, -2, "write");
    lua_setglobal(L_, "log");
    lua_newtable(L_);
    lua_pushcfunction(L_, &l_settings_get); lua_setfield(L_, -2, "get");
    lua_pushcfunction(L_, &l_settings_set); lua_setfield(L_, -2, "set");
    lua_setglobal(L_, "settings");
    lua_pushcfunction(L_, &l_on); lua_setglobal(L_, "on");
    lua_newtable(L_);
    lua_pushcfunction(L_, &l_hook_pre); lua_setfield(L_, -2, "pre");
    lua_pushcfunction(L_, &l_hook_list); lua_setfield(L_, -2, "list");
    lua_pushcfunction(L_, &l_hook_unavailable); lua_setfield(L_, -2, "post");
    lua_pushcfunction(L_, &l_hook_unavailable); lua_setfield(L_, -2, "replace");
    lua_setglobal(L_, "hook");
    lua_newtable(L_);
    const std::pair<const char*, lua_CFunction> widgets[] = {
        {"draw", &l_ui_draw}, {"text", &l_ui_text}, {"button", &l_ui_button}, {"checkbox", &l_ui_checkbox}, {"slider_int", &l_ui_slider_int},
        {"slider_float", &l_ui_slider_float}, {"combo", &l_ui_combo}, {"input_text", &l_ui_input_text}, {"separator", &l_ui_separator},
        {"same_line", &l_ui_same_line}, {"collapsing_header", &l_ui_header}, {"tooltip", &l_ui_tooltip}, {"color_edit", &l_ui_color_edit}};
    for (const auto& [name, fn] : widgets) { lua_pushcfunction(L_, fn); lua_setfield(L_, -2, name); }
    lua_setglobal(L_, "ui");
}

void LuaRuntime::install_uc() {
    // Group the registry by domain: "player.magazine" -> domain "player", member "magazine".
    std::map<std::string, std::vector<const Member*>> by_domain;
    for (const auto& path : ctx_.api.paths()) {
        const auto* member = ctx_.api.find(path);
        by_domain[path.substr(0, path.find('.'))].push_back(member);
    }
    lua_newtable(L_);   // uc
    for (const auto& [domain, members] : by_domain) {
        DomainInfo& info = domains_.emplace_back();
        info.name = domain;
        for (const auto* m : members) {
            if (!m->scope.empty()) info.scope = m->scope;
            if (m->get || m->set) info.properties[m->path.substr(domain.size() + 1)] = m;
        }
        const bool scoped = !info.scope.empty();
        lua_newtable(L_);   // metatable (shared by the domain table or by every handle)
        lua_pushlightuserdata(L_, &info); lua_pushcclosure(L_, &l_prop_index, 1); lua_setfield(L_, -2, "__index");
        lua_pushlightuserdata(L_, &info); lua_pushcclosure(L_, &l_prop_newindex, 1); lua_setfield(L_, -2, "__newindex");
        if (scoped) {
            lua_pushvalue(L_, -1);
            lua_setfield(L_, LUA_REGISTRYINDEX, ("uc_" + domain).c_str());
            lua_pop(L_, 1);
            lua_pushlightuserdata(L_, &info); lua_pushcclosure(L_, &l_scoped_handle, 1);
            lua_setfield(L_, -2, domain.c_str());
            continue;
        }
        lua_newtable(L_);   // the domain table: [metatable][table]
        for (const auto* m : members) {
            if (!m->call) continue;
            lua_pushlightuserdata(L_, const_cast<Member*>(m));
            lua_pushcclosure(L_, &l_api_call, 1);
            lua_setfield(L_, -2, m->path.substr(domain.size() + 1).c_str());
        }
        lua_insert(L_, -2);        // [table][metatable]
        lua_setmetatable(L_, -2);  // [table]
        lua_setfield(L_, -2, domain.c_str());
    }
    lua_setglobal(L_, "uc");
}

}  // namespace

std::unique_ptr<ModRuntime> make_lua_runtime(ModContext& context) { return std::make_unique<LuaRuntime>(context); }

}  // namespace uc::mods

// Called from the pattern matcher of the bundled Lua (patched in modding/CMakeLists.txt) every few thousand match steps.
void uc_lua_poll(lua_State* L) {
    static thread_local unsigned steps = 0;
    if ((++steps & 0x3FFF) == 0) uc::mods::LuaRuntime::check_budget(L);
}
