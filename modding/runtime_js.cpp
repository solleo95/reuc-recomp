// JavaScript runtime for mods on QuickJS-ng. No std/os modules and no timers are created: the only host objects are the mod API globals.
// Entry files and everything they import are ES modules loaded from inside the mod folder only.
#include "mod_paths.h"
#include "runtime.h"
#include <quickjs.h>
#include <deque>
#include <map>

namespace fs = std::filesystem;

namespace uc::mods {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kMemoryCap = 64u << 20;
constexpr size_t kStackCap = 256u << 10;   // measured from where the callback starts; the threads that call in have at least 1 MiB
constexpr auto kLoadBudget = std::chrono::seconds(2);
constexpr size_t kMaxJsonBytes = 1u << 20;

struct DomainInfo {
    std::string name;
    std::string scope;   // empty for a global domain
    std::vector<std::pair<std::string, const Member*>> properties;
};

// Owns one JSValue.
class Val {
public:
    Val(JSContext* ctx, JSValue v) : ctx_(ctx), v_(v) {}
    Val(const Val&) = delete;
    Val& operator=(const Val&) = delete;
    ~Val() { JS_FreeValue(ctx_, v_); }
    JSValue get() const { return v_; }
    JSValue release() { JSValue v = v_; v_ = JS_UNDEFINED; return v; }
private:
    JSContext* ctx_;
    JSValue v_;
};

std::string to_text(JSContext* ctx, JSValueConst v) {
    size_t n = 0;
    const char* s = JS_ToCStringLen(ctx, &n, v);
    std::string text = s ? std::string(s, n) : std::string("(unprintable value)");
    if (s) JS_FreeCString(ctx, s);
    return text;
}

// An error value as one line plus its stack (file:line of the script).
std::string describe_error(JSContext* ctx, JSValueConst error) {
    std::string text = to_text(ctx, error);
    if (text == "InternalError") text = "InternalError: out of memory";   // the engine raises it without a message when the memory cap is hit (a message would need memory)
    if (JS_IsObject(error)) {
        Val stack(ctx, JS_GetPropertyStr(ctx, error, "stack"));
        if (JS_IsString(stack.get())) text += "\n" + to_text(ctx, stack.get());
    }
    return text;
}

std::string take_exception(JSContext* ctx) {
    Val exception(ctx, JS_GetException(ctx));
    return describe_error(ctx, exception.get());
}

Value from_js(JSContext* ctx, JSValueConst v) {
    if (JS_IsUndefined(v)) return Value(nullptr);
    Val json(ctx, JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED));
    if (JS_IsException(json.get())) throw ApiError("value cannot be passed to the game API: " + take_exception(ctx));
    if (!JS_IsString(json.get())) throw ApiError("a function or symbol cannot be passed to the game API");
    const std::string text = to_text(ctx, json.get());
    if (text.size() > kMaxJsonBytes) throw ApiError("value passed to the game API is too large");
    const Value parsed = Value::parse(text, nullptr, false);
    if (parsed.is_discarded()) throw ApiError("value cannot be passed to the game API");
    return parsed;
}

JSValue to_js(JSContext* ctx, const Value& v) {
    const std::string text = v.dump();
    return JS_ParseJSON(ctx, text.data(), text.size(), "<api>");
}

}  // namespace

class JsRuntime : public ModRuntime {
public:
    explicit JsRuntime(ModContext& context) : ctx_(context) {
        rt_ = JS_NewRuntime();
        if (!rt_) throw std::runtime_error("cannot create a JavaScript runtime");
        JS_SetMemoryLimit(rt_, kMemoryCap);
        JS_SetMaxStackSize(rt_, kStackCap);
        JS_SetInterruptHandler(rt_, &JsRuntime::interrupt, this);
        JS_SetModuleLoaderFunc(rt_, &JsRuntime::normalize_module, &JsRuntime::load_module, this);
        js_ = JS_NewContext(rt_);
        if (!js_) throw std::runtime_error("cannot create a JavaScript context");
        JS_SetContextOpaque(js_, this);
        deadline_ = Clock::time_point::max();
        install_globals();
    }
    ~JsRuntime() override {
        for (auto& [event, list] : handlers_) for (auto& f : list) JS_FreeValue(js_, f);
        for (auto& f : hook_fns_) JS_FreeValue(js_, f);
        JS_FreeValue(js_, draw_);
        JS_FreeContext(js_);
        JS_FreeRuntime(rt_);
    }

    std::string name() const override { return "JavaScript (QuickJS-ng)"; }
    bool has_ui() const override { return !JS_IsUndefined(draw_); }

    CallResult load(const fs::path& mod_root, const std::string& entry) override {
        root_ = mod_root;
        const auto path = confine(root_, entry);
        const auto text = path ? read_file(*path) : std::nullopt;
        if (!text) return {false, "cannot read " + entry, false};
        enter();
        deadline_ = Clock::now() + kLoadBudget;
        Val result(js_, JS_Eval(js_, text->data(), text->size(), entry.c_str(), JS_EVAL_TYPE_MODULE));
        CallResult out = finish(result.get(), true);
        deadline_ = Clock::time_point::max();
        return out;
    }

    CallResult fire(std::string_view event, const Value& payload, Budget budget) override {
        const auto at = handlers_.find(std::string(event));
        if (at == handlers_.end() || at->second.empty()) return {};
        std::vector<JSValue> handlers;   // a handler may register more handlers; keep our own references
        for (const auto& f : at->second) handlers.push_back(JS_DupValue(js_, f));
        CallResult out;
        enter();
        deadline_ = Clock::now() + budget.limit;
        for (const auto& f : handlers) {
            Val argument(js_, to_js(js_, payload));
            JSValue argv[1] = {argument.get()};
            Val result(js_, JS_Call(js_, f, JS_UNDEFINED, 1, argv));
            out = finish(result.get(), false);
            if (!out.ok) break;
        }
        deadline_ = Clock::time_point::max();
        for (const auto& f : handlers) JS_FreeValue(js_, f);
        return out;
    }

    CallResult draw_ui(Budget budget) override {
        if (JS_IsUndefined(draw_)) return {};
        enter();
        deadline_ = Clock::now() + budget.limit;
        in_ui_ = true;
        Val result(js_, JS_Call(js_, draw_, JS_UNDEFINED, 0, nullptr));
        const CallResult out = finish(result.get(), false);
        in_ui_ = false;
        deadline_ = Clock::time_point::max();
        return out;
    }

private:
    // Start of every entry from the host: the calling thread differs between calls, and QuickJS measures its stack limit from the thread that last set the top.
    void enter() {
        JS_UpdateStackTop(rt_);
        over_budget_ = false;
    }

    // The value a call or evaluation returned: an exception, a rejected promise (module evaluation) or fine. Also drains the promise jobs it started.
    CallResult finish(JSValueConst result, bool module_evaluation) {
        auto failure = [&](std::string text) { return CallResult{false, std::move(text), over_budget_}; };
        if (JS_IsException(result)) return failure(take_exception(js_));
        for (int jobs = 0; jobs < 100000; ++jobs) {
            JSContext* where;
            const int status = JS_ExecutePendingJob(rt_, &where);
            if (status == 0) break;
            if (status < 0) return failure(take_exception(where));
        }
        if (module_evaluation) {
            const auto state = JS_PromiseState(js_, result);
            if (state == JS_PROMISE_REJECTED) {
                Val reason(js_, JS_PromiseResult(js_, result));
                return failure(describe_error(js_, reason.get()));
            }
            if (state == JS_PROMISE_PENDING) return failure(over_budget_ ? "script exceeded its time budget" : "the script's top-level await never finished");
        }
        return {};
    }

    static JsRuntime* me(JSContext* c) { return static_cast<JsRuntime*>(JS_GetContextOpaque(c)); }

    static int interrupt(JSRuntime*, void* opaque) {
        auto* self = static_cast<JsRuntime*>(opaque);
        if (Clock::now() > self->deadline_) { self->over_budget_ = true; return 1; }   // not catchable by the script
        return 0;
    }

    // ---- modules: relative imports inside the mod folder only ----
    static char* normalize_module(JSContext* c, const char* base, const char* name, void*) {
        const std::string wanted = name;
        if (wanted.rfind("./", 0) != 0 && wanted.rfind("../", 0) != 0) {
            JS_ThrowReferenceError(c, "only relative imports like './file.js' are allowed (got '%s')", name);
            return nullptr;
        }
        if (wanted.find_first_of("\\:") != std::string::npos) { JS_ThrowReferenceError(c, "bad module path '%s'", name); return nullptr; }
        const std::string from = base;
        std::vector<std::string> parts;
        const auto split = [&](const std::string& path) {
            size_t start = 0;
            while (start <= path.size()) {
                const auto slash = path.find('/', start);
                const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
                if (part == "..") { if (parts.empty()) return false; parts.pop_back(); }
                else if (!part.empty() && part != ".") parts.push_back(part);
                if (slash == std::string::npos) break;
                start = slash + 1;
            }
            return true;
        };
        const auto slash = from.rfind('/');
        const bool ok = (slash == std::string::npos || split(from.substr(0, slash))) && split(wanted);
        if (!ok || parts.empty()) { JS_ThrowReferenceError(c, "import '%s' leaves the mod folder", name); return nullptr; }
        std::string joined;
        for (const auto& p : parts) joined += (joined.empty() ? "" : "/") + p;
        return js_strdup(c, joined.c_str());
    }

    static JSModuleDef* load_module(JSContext* c, const char* name, void* opaque) {
        auto* self = static_cast<JsRuntime*>(opaque);
        const auto path = confine(self->root_, name);
        const auto text = path ? read_file(*path) : std::nullopt;
        if (!text) { JS_ThrowReferenceError(c, "could not load module '%s'", name); return nullptr; }
        Val compiled(c, JS_Eval(c, text->data(), text->size(), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY));
        if (JS_IsException(compiled.get())) return nullptr;
        return static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled.get()));
    }

    // ---- API calls ----
    static JSValue throw_text(JSContext* c, const std::string& text) { return JS_ThrowInternalError(c, "%s", text.c_str()); }

    static JSValue api_call(JSContext* c, JSValueConst, int argc, JSValueConst* argv, int magic, JSValueConst*) {
        const Member* member = me(c)->members_[size_t(magic)];
        try {
            if (size_t(argc) > member->arg_names.size()) throw ApiError(member->path + " takes " + std::to_string(member->arg_names.size()) + " argument(s)");
            Value args = Value::object();
            for (int i = 0; i < argc; ++i) args[member->arg_names[size_t(i)]] = from_js(c, argv[i]);
            return to_js(c, member->call(args));
        } catch (const std::exception& e) { return throw_text(c, e.what()); }
    }

    static Value scope_args(JSContext* c, const DomainInfo& info, JSValueConst* data) {
        Value args = Value::object();
        if (!info.scope.empty()) args[info.scope] = from_js(c, data[0]);
        return args;
    }

    static JSValue prop_get(JSContext* c, JSValueConst, int, JSValueConst*, int magic, JSValueConst* data) {
        JsRuntime* self = me(c);
        const auto& [info_index, member_index] = self->property_refs_[size_t(magic)];
        try { return to_js(c, self->members_[member_index]->get(scope_args(c, self->domains_[info_index], data))); }
        catch (const std::exception& e) { return throw_text(c, e.what()); }
    }
    static JSValue prop_set(JSContext* c, JSValueConst, int argc, JSValueConst* argv, int magic, JSValueConst* data) {
        JsRuntime* self = me(c);
        const auto& [info_index, member_index] = self->property_refs_[size_t(magic)];
        try {
            self->members_[member_index]->set(scope_args(c, self->domains_[info_index], data), from_js(c, argc > 0 ? argv[0] : JS_UNDEFINED));
            return JS_UNDEFINED;
        } catch (const std::exception& e) { return throw_text(c, e.what()); }
    }

    // Defines the accessor pair for every property of the domain on `target`; `scope` is stored in the closures' data.
    void define_properties(JSValueConst target, size_t info_index, JSValueConst scope) {
        const DomainInfo& info = domains_[info_index];
        for (const auto& [name, member] : info.properties) {
            property_refs_.emplace_back(info_index, member_index(member));
            const int magic = int(property_refs_.size() - 1);
            JSValueConst data[1] = {scope};
            JSValue getter = member->get ? JS_NewCFunctionData(js_, &JsRuntime::prop_get, 0, magic, 1, data) : JS_UNDEFINED;
            JSValue setter = member->set ? JS_NewCFunctionData(js_, &JsRuntime::prop_set, 1, magic, 1, data) : JS_UNDEFINED;
            JSAtom atom = JS_NewAtom(js_, name.c_str());
            JS_DefinePropertyGetSet(js_, target, atom, getter, setter, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
            JS_FreeAtom(js_, atom);
        }
    }

    static JSValue scoped_handle(JSContext* c, JSValueConst, int argc, JSValueConst* argv, int magic, JSValueConst*) {
        JsRuntime* self = me(c);
        const DomainInfo& info = self->domains_[size_t(magic)];
        if (argc != 1) return JS_ThrowTypeError(c, "uc.%s() takes one argument: the %s", info.name.c_str(), info.scope.c_str());
        JSValue handle = JS_NewObject(c);
        self->define_properties(handle, size_t(magic), argv[0]);
        return handle;
    }

    size_t member_index(const Member* m) {
        for (size_t i = 0; i < members_.size(); ++i) if (members_[i] == m) return i;
        members_.push_back(m);
        return members_.size() - 1;
    }

    // ---- globals ----
    static std::string join_arguments(JSContext* c, int argc, JSValueConst* argv) {
        std::string line;
        for (int i = 0; i < argc; ++i) { if (i) line += '\t'; line += to_text(c, argv[i]); }
        return line;
    }
    static JSValue log_write(JSContext* c, JSValueConst, int argc, JSValueConst* argv) { me(c)->ctx_.log(join_arguments(c, argc, argv)); return JS_UNDEFINED; }

    static JSValue settings_get(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
        if (argc < 1) return JS_ThrowTypeError(c, "settings.get(key) needs a key");
        return to_js(c, me(c)->ctx_.settings.get(to_text(c, argv[0])));
    }
    static JSValue settings_set(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
        if (argc < 2) return JS_ThrowTypeError(c, "settings.set(key, value) needs two arguments");
        try { return JS_NewBool(c, me(c)->ctx_.settings.set(to_text(c, argv[0]), from_js(c, argv[1]))); }
        catch (const std::exception& e) { return throw_text(c, e.what()); }
    }

    static JSValue on(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
        if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(c, argv[1])) return JS_ThrowTypeError(c, "on(event, handler) needs an event name and a function");
        auto& list = me(c)->handlers_[to_text(c, argv[0])];
        if (list.size() >= 256) return JS_ThrowInternalError(c, "too many handlers for '%s'", to_text(c, argv[0]).c_str());
        list.push_back(JS_DupValue(c, argv[1]));
        return JS_UNDEFINED;
    }

    // hook.pre(name, fn): fn gets an object with r3..r10 (integers) and f1..f8 (numbers) and may change them.
    CallResult call_hook(size_t index, HookRegs& regs, Budget budget) {
        enter();
        deadline_ = Clock::now() + budget.limit;
        Val object(js_, JS_NewObject(js_));
        for (int i = 0; i < 8; ++i) {
            JS_SetPropertyStr(js_, object.get(), ("r" + std::to_string(3 + i)).c_str(), JS_NewInt64(js_, regs.r[i]));
            JS_SetPropertyStr(js_, object.get(), ("f" + std::to_string(1 + i)).c_str(), JS_NewFloat64(js_, regs.f[i]));
        }
        JSValue argv[1] = {object.get()};
        Val result(js_, JS_Call(js_, hook_fns_[index], JS_UNDEFINED, 1, argv));
        CallResult out = finish(result.get(), false);
        deadline_ = Clock::time_point::max();
        if (!out.ok) return out;
        for (int i = 0; i < 8; ++i) {
            Val r(js_, JS_GetPropertyStr(js_, object.get(), ("r" + std::to_string(3 + i)).c_str()));
            int64_t integer = 0;
            if (JS_IsNumber(r.get()) && JS_ToInt64(js_, &integer, r.get()) == 0) regs.r[i] = uint32_t(integer);
            Val f(js_, JS_GetPropertyStr(js_, object.get(), ("f" + std::to_string(1 + i)).c_str()));
            double number = 0;
            if (JS_IsNumber(f.get()) && JS_ToFloat64(js_, &number, f.get()) == 0) regs.f[i] = number;
        }
        return out;
    }
    static JSValue hook_pre(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
        if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(c, argv[1])) return JS_ThrowTypeError(c, "hook.pre(name, callback) needs a function name and a function");
        JsRuntime* self = me(c);
        if (!self->ctx_.hooks) return JS_ThrowInternalError(c, "hooks are not available here (they need \"touches_gameplay\": true in mod.json)");
        const std::string name = to_text(c, argv[0]);
        self->hook_fns_.push_back(JS_DupValue(c, argv[1]));
        const size_t index = self->hook_fns_.size() - 1;
        if (!self->ctx_.hooks->add_pre(self, name, [self, index](HookRegs& regs, Budget budget) { return self->call_hook(index, regs, budget); })) {
            JS_FreeValue(c, self->hook_fns_.back());
            self->hook_fns_.pop_back();
            return JS_ThrowInternalError(c, "'%s' is not a hookable function (see hook.list())", name.c_str());
        }
        return JS_UNDEFINED;
    }
    static JSValue hook_list(JSContext* c, JSValueConst, int, JSValueConst*) {
        const auto names = me(c)->ctx_.hooks ? me(c)->ctx_.hooks->names() : std::vector<std::string>();
        JSValue array = JS_NewArray(c);
        for (size_t i = 0; i < names.size(); ++i) JS_SetPropertyUint32(c, array, uint32_t(i), JS_NewStringLen(c, names[i].data(), names[i].size()));
        return array;
    }
    static JSValue hook_unavailable(JSContext* c, JSValueConst, int, JSValueConst*) {
        return JS_ThrowInternalError(c, "hook.post and hook.replace are not available in this version (see the API reference)");
    }

    static JSValue ui_draw(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
        if (argc < 1 || !JS_IsFunction(c, argv[0])) return JS_ThrowTypeError(c, "ui.draw(callback) needs a function");
        JsRuntime* self = me(c);
        JS_FreeValue(c, self->draw_);
        self->draw_ = JS_DupValue(c, argv[0]);
        return JS_UNDEFINED;
    }

    enum Widget { Text, Button, Checkbox, SliderInt, SliderFloat, Combo, InputText, Separator, SameLine, Header, Tooltip, ColorEdit };
    static JSValue ui_widget(JSContext* c, JSValueConst, int argc, JSValueConst* argv, int magic) {
        JsRuntime* self = me(c);
        if (!self->in_ui_ || !self->ctx_.ui) return JS_ThrowInternalError(c, "ui functions can only be used inside the function given to ui.draw");
        UiApi& ui = *self->ctx_.ui;
        const auto text = [&](int i) { return i < argc ? to_text(c, argv[i]) : std::string(); };
        const auto number = [&](int i) { double d = 0; if (i < argc) JS_ToFloat64(c, &d, argv[i]); return d; };
        try {
            switch (magic) {
                case Text: ui.text(text(0)); return JS_UNDEFINED;
                case Button: return JS_NewBool(c, ui.button(text(0)));
                case Checkbox: return JS_NewBool(c, ui.checkbox(text(0), argc > 1 && JS_ToBool(c, argv[1]) > 0));
                case SliderInt: return JS_NewInt32(c, ui.slider_int(text(0), int(number(1)), int(number(2)), int(number(3))));
                case SliderFloat: return JS_NewFloat64(c, ui.slider_float(text(0), number(1), number(2), number(3)));
                case Combo: {
                    std::vector<std::string> items;
                    if (argc > 2) {
                        Val length(c, JS_GetPropertyStr(c, argv[2], "length"));
                        int32_t n = 0; JS_ToInt32(c, &n, length.get());
                        for (int32_t i = 0; i < n && i < 1000; ++i) { Val item(c, JS_GetPropertyUint32(c, argv[2], uint32_t(i))); items.push_back(to_text(c, item.get())); }
                    }
                    return JS_NewInt32(c, ui.combo(text(0), int(number(1)), items));
                }
                case InputText: { const auto result = ui.input_text(text(0), text(1)); return JS_NewStringLen(c, result.data(), result.size()); }
                case Separator: ui.separator(); return JS_UNDEFINED;
                case SameLine: ui.same_line(); return JS_UNDEFINED;
                case Header: return JS_NewBool(c, ui.collapsing_header(text(0)));
                case Tooltip: ui.tooltip(text(0)); return JS_UNDEFINED;
                case ColorEdit: {
                    std::array<float, 4> rgba{};
                    for (uint32_t i = 0; i < 4 && argc > 1; ++i) { Val item(c, JS_GetPropertyUint32(c, argv[1], i)); double d = 0; JS_ToFloat64(c, &d, item.get()); rgba[i] = float(d); }
                    rgba = ui.color_edit(text(0), rgba);
                    JSValue array = JS_NewArray(c);
                    for (uint32_t i = 0; i < 4; ++i) JS_SetPropertyUint32(c, array, i, JS_NewFloat64(c, rgba[i]));
                    return array;
                }
            }
        } catch (const std::exception& e) { return throw_text(c, e.what()); }
        return JS_UNDEFINED;
    }

    void install_globals() {
        JSValue global = JS_GetGlobalObject(js_);
        const auto object_with = [&](const char* name, std::initializer_list<std::pair<const char*, JSCFunction*>> functions) {
            JSValue object = JS_NewObject(js_);
            for (const auto& [fn_name, fn] : functions) JS_SetPropertyStr(js_, object, fn_name, JS_NewCFunction(js_, fn, fn_name, 1));
            JS_SetPropertyStr(js_, global, name, object);
        };
        object_with("log", {{"write", &JsRuntime::log_write}});
        object_with("settings", {{"get", &JsRuntime::settings_get}, {"set", &JsRuntime::settings_set}});
        JS_SetPropertyStr(js_, global, "on", JS_NewCFunction(js_, &JsRuntime::on, "on", 2));
        object_with("hook", {{"pre", &JsRuntime::hook_pre}, {"list", &JsRuntime::hook_list}, {"post", &JsRuntime::hook_unavailable}, {"replace", &JsRuntime::hook_unavailable}});
        JSValue ui = JS_NewObject(js_);
        JS_SetPropertyStr(js_, ui, "draw", JS_NewCFunction(js_, &JsRuntime::ui_draw, "draw", 1));
        const std::pair<const char*, int> widgets[] = {{"text", Text}, {"button", Button}, {"checkbox", Checkbox}, {"slider_int", SliderInt}, {"slider_float", SliderFloat},
                                                       {"combo", Combo}, {"input_text", InputText}, {"separator", Separator}, {"same_line", SameLine},
                                                       {"collapsing_header", Header}, {"tooltip", Tooltip}, {"color_edit", ColorEdit}};
        for (const auto& [name, magic] : widgets) JS_SetPropertyStr(js_, ui, name, JS_NewCFunctionMagic(js_, &JsRuntime::ui_widget, name, 1, JS_CFUNC_generic_magic, magic));
        JS_SetPropertyStr(js_, global, "ui", ui);

        std::map<std::string, std::vector<const Member*>> by_domain;
        for (const auto& path : ctx_.api.paths()) by_domain[path.substr(0, path.find('.'))].push_back(ctx_.api.find(path));
        JSValue uc = JS_NewObject(js_);
        for (const auto& [domain, members] : by_domain) {
            domains_.emplace_back();
            const size_t index = domains_.size() - 1;
            DomainInfo& info = domains_.back();
            info.name = domain;
            for (const auto* m : members) {
                if (!m->scope.empty()) info.scope = m->scope;
                if (m->get || m->set) info.properties.emplace_back(m->path.substr(domain.size() + 1), m);
            }
            if (!info.scope.empty()) {
                JS_SetPropertyStr(js_, uc, domain.c_str(), JS_NewCFunctionData(js_, &JsRuntime::scoped_handle, 1, int(index), 0, nullptr));
                continue;
            }
            JSValue object = JS_NewObject(js_);
            for (const auto* m : members) {
                if (!m->call) continue;
                JS_SetPropertyStr(js_, object, m->path.substr(domain.size() + 1).c_str(),
                                  JS_NewCFunctionData(js_, &JsRuntime::api_call, int(m->arg_names.size()), int(member_index(m)), 0, nullptr));
            }
            define_properties(object, index, JS_UNDEFINED);
            JS_SetPropertyStr(js_, uc, domain.c_str(), object);
        }
        JS_SetPropertyStr(js_, global, "uc", uc);
        JS_FreeValue(js_, global);
    }

    ModContext& ctx_;
    JSRuntime* rt_ = nullptr;
    JSContext* js_ = nullptr;
    fs::path root_;
    Clock::time_point deadline_;
    bool over_budget_ = false;
    bool in_ui_ = false;
    JSValue draw_ = JS_UNDEFINED;
    std::map<std::string, std::vector<JSValue>> handlers_;
    std::vector<JSValue> hook_fns_;
    std::deque<DomainInfo> domains_;
    std::vector<const Member*> members_;                         // magic of an api_call: index here
    std::vector<std::pair<size_t, size_t>> property_refs_;       // magic of a property accessor: (domain index, member index)
};

std::unique_ptr<ModRuntime> make_js_runtime(ModContext& context) { return std::make_unique<JsRuntime>(context); }

}  // namespace uc::mods
