#include "test_protocol.h"
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
namespace uc {
namespace {
struct Value;
using Object = std::map<std::string, Value, std::less<>>;
struct Value { enum class Type { number, boolean, string, object } type; double number = 0; bool boolean = false; std::string text; std::shared_ptr<Object> object; };
struct Reader {
    std::string_view s; size_t p = 0;
    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p; }
    bool eat(char c) { ws(); if (p < s.size() && s[p] == c) { ++p; return true; } return false; }
    std::string string() {
        if (!eat('"')) throw std::runtime_error("expected string");
        std::string out;
        while (p < s.size() && s[p] != '"') {
            char c = s[p++];
            if (c == '\\') { if (p >= s.size()) break; char e = s[p++]; if (e == '"' || e == '\\' || e == '/') out += e; else if (e == 'n') out += '\n'; else if (e == 't') out += '\t'; else throw std::runtime_error("unsupported escape"); }
            else if (uint8_t(c) < 0x20) throw std::runtime_error("control character in string");
            else out += c;
        }
        if (!eat('"')) throw std::runtime_error("unterminated string");
        return out;
    }
    Value value(int depth) {
        ws();
        if (p >= s.size()) throw std::runtime_error("unexpected end");
        if (s[p] == '"') return {Value::Type::string, 0, false, string(), {}};
        if (s[p] == '{') { if (depth >= 2) throw std::runtime_error("nesting too deep"); return {Value::Type::object, 0, false, {}, std::make_shared<Object>(object(depth + 1))}; }
        if (s.substr(p, 4) == "true") { p += 4; return {Value::Type::boolean, 0, true, {}, {}}; }
        if (s.substr(p, 5) == "false") { p += 5; return {Value::Type::boolean, 0, false, {}, {}}; }
        double d = 0; auto r = std::from_chars(s.data() + p, s.data() + s.size(), d);
        if (r.ec != std::errc{} || !std::isfinite(d)) throw std::runtime_error("bad value");
        p = size_t(r.ptr - s.data());
        return {Value::Type::number, d, false, {}, {}};
    }
    Object object(int depth) {
        if (!eat('{')) throw std::runtime_error("expected object");
        Object out;
        if (eat('}')) return out;
        do {
            auto key = string();
            if (!eat(':')) throw std::runtime_error("expected ':'");
            if (!out.emplace(key, value(depth)).second) throw std::runtime_error("duplicate key " + key);
        } while (eat(','));
        if (!eat('}')) throw std::runtime_error("expected '}'");
        return out;
    }
};
double number(const Object& o, std::string_view key, double low, double high, bool integer) {
    auto it = o.find(key);
    if (it == o.end() || it->second.type != Value::Type::number) throw std::runtime_error(std::string(key) + " must be a number");
    const double v = it->second.number;
    if (v < low || v > high || (integer && v != std::floor(v))) throw std::runtime_error(std::string(key) + " out of range");
    return v;
}
bool boolean(const Object& o, std::string_view key) {
    auto it = o.find(key);
    if (it == o.end() || it->second.type != Value::Type::boolean) throw std::runtime_error(std::string(key) + " must be true/false");
    return it->second.boolean;
}
void only(const Object& o, std::initializer_list<std::string_view> keys) {
    for (const auto& [k, v] : o) {
        bool known = false; for (auto key : keys) known |= key == k;
        if (!known) throw std::runtime_error("unknown field " + k);
    }
}
}  // namespace
std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (char c : text) { if (c == '"' || c == '\\') { out += '\\'; out += c; } else if (c == '\n') out += "\\n"; else if (uint8_t(c) < 0x20) out += ' '; else out += c; }
    return out + "\"";
}
std::string test_reply_ok(uint64_t id, std::string_view extra) { return "{\"id\":" + std::to_string(id) + ",\"ok\":true" + (extra.empty() ? "" : "," + std::string(extra)) + "}"; }
std::string test_reply_error(uint64_t id, std::string_view message) { return "{\"id\":" + std::to_string(id) + ",\"error\":" + json_string(message) + "}"; }
std::variant<TestRequest, TestParseError> parse_test_request(std::string_view line) {
    uint64_t id = 0;
    try {
        if (line.size() > 4096) throw std::runtime_error("command line too long");
        Reader reader{line}; auto root = reader.object(0); reader.ws();
        if (reader.p != line.size()) throw std::runtime_error("trailing bytes");
        if (root.contains("id")) id = uint64_t(number(root, "id", 1, 9007199254740991.0, true));
        else throw std::runtime_error("id required");
        auto cmd = root.find("cmd");
        if (cmd == root.end() || cmd->second.type != Value::Type::string) throw std::runtime_error("cmd required");
        TestRequest r; r.id = id;
        const auto& name = cmd->second.text;
        if (name == "capture" || name == "state" || name == "quit" || name == "draws") {
            only(root, {"id", "cmd"});
            r.kind = name == "capture" ? TestRequestKind::capture : name == "state" ? TestRequestKind::state : name == "draws" ? TestRequestKind::draws : TestRequestKind::quit;
        } else if (name == "speed") {
            only(root, {"id", "cmd", "factor"}); r.kind = TestRequestKind::speed; r.factor = uint32_t(number(root, "factor", 1, 8, true));
        } else if (name == "peek") {   // read-only guest memory window for diagnostics and reverse engineering
            only(root, {"id", "cmd", "addr", "size"}); r.kind = TestRequestKind::peek;
            r.addr = uint32_t(number(root, "addr", 0, 4294967295.0, true)); r.size = uint32_t(number(root, "size", 1, 262144, true));
        } else if (name == "input") {
            only(root, {"id", "cmd", "at_frame", "hold_frames", "kpad", "key", "mouse", "wheel", "motion"});
            r.kind = TestRequestKind::input; auto& in = r.input;
            if (root.contains("at_frame")) in.at_frame = uint64_t(number(root, "at_frame", 0, 9007199254740991.0, true));
            if (root.contains("hold_frames")) in.hold_frames = uint32_t(number(root, "hold_frames", 1, 600, true));
            int devices = 0; const Object* body = nullptr;
            for (auto [key, device] : {std::pair{"kpad", TestDevice::kpad}, {"key", TestDevice::key}, {"mouse", TestDevice::mouse}, {"wheel", TestDevice::wheel}, {"motion", TestDevice::motion}}) {
                auto it = root.find(key);
                if (it == root.end()) continue;
                if (it->second.type != Value::Type::object) throw std::runtime_error(std::string(key) + " must be an object");
                ++devices; in.device = device; body = it->second.object.get();
            }
            if (devices != 1) throw std::runtime_error("input needs exactly one device");
            switch (in.device) {
            case TestDevice::kpad:
                only(*body, {"x", "y", "buttons"});
                in.x = float(number(*body, "x", -1, 1, false)); in.y = float(number(*body, "y", -1, 1, false));
                in.buttons = uint32_t(number(*body, "buttons", 0, 0xFFFF, true));
                if (in.buttons & ~0x1C10u) throw std::runtime_error("buttons outside A/B/plus/minus");
                break;
            case TestDevice::key: only(*body, {"code", "down"}); in.code = uint32_t(number(*body, "code", 1, 511, true)); in.down = boolean(*body, "down"); break;
            case TestDevice::mouse: only(*body, {"button", "down"}); in.code = uint32_t(number(*body, "button", 1, 255, true)); in.down = boolean(*body, "down"); break;
            case TestDevice::wheel: only(*body, {"delta"}); in.delta = int(number(*body, "delta", -256, 256, true)); if (!in.delta) throw std::runtime_error("delta must be nonzero"); break;
            case TestDevice::motion: only(*body, {"x", "y"}); in.x = float(number(*body, "x", -4096, 4096, false)); in.y = float(number(*body, "y", -4096, 4096, false)); break;
            }
            if ((in.device == TestDevice::wheel || in.device == TestDevice::motion) && in.hold_frames != 1) throw std::runtime_error("hold_frames applies to kpad/key/mouse only");
        } else throw std::runtime_error("unknown cmd " + name);
        return r;
    } catch (const std::exception& e) { return TestParseError{id, e.what()}; }
}
}  // namespace uc
