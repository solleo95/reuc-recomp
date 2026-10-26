# Agent Test Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** an in-game test server plus a Python client so agents reach scenes, drive controls and measure performance reliably and fast, without the owner.

**Architecture:** `uc.exe --test-server <name>` opens two named pipes (commands in, events out) carrying JSON lines. The guest thread publishes a per-KPAD-sample game state snapshot; the main thread applies frame-scheduled input through the existing `NativeInput` test API and streams `frame` events. A virtual clock scales all runtime time for acceleration. `scripts/uctest` (stdlib Python) launches the game, waits on state predicates, navigates menus with verification and runs scenarios from a CLI.

**Tech Stack:** C++20/clang-cl, Win32 named pipes, existing uc_runtime/NativeInput; Python 3.13 stdlib (`unittest`, `subprocess`, `threading`, `json`).

**Spec:** `docs/superpowers/specs/2026-10-15-agent-test-harness-design.md`

## Global Constraints

- Test mode only; normal play unchanged without `--test-server`.
- Visible window shown without activation; no SendInput, hooks or cursor moves; isolated `--save-root` required.
- Acceleration only by state waits and the virtual clock (speed 1..8); no scene warps, no hidden window.
- Input enters only through `NativeInput::apply_test_command(sequence,x,y,held)` / `apply_test_event(sequence,HostEvent,width,height)`.
- No third-party dependencies (C++ and Python standard libraries only).
- Protocol limits: command line ≤ 4096 bytes, ≤ 256 queued commands, `hold_frames` 1..600, coordinates finite in [-1,1], KPAD buttons ⊆ 0x1C10, speed 1..8; violations reply with an error and change nothing.
- Files are never deleted without the owner's request; commits only on request (owner said: no commits for now).
- Ruling (plan vs spec): two unidirectional pipes `\\.\pipe\uc-test-<name>-cmd` and `-evt` instead of one duplex pipe — synchronous Win32/Python file handles serialize concurrent read/write on one handle; protocol unchanged. Render skip at speed > 1 is not implemented in v1; achieved speed is reported (`frame.retraces` rate) and the gap goes to tech debt if presentation limits it.
- Ruling (plan vs spec): no `log`/`fatal` pipe events in v1 — the client reads the tail of `run.log` and the newest `debug/crash-*.txt` itself when the process dies (same information, no second channel for crash paths); player health/ammo stay out until their offsets are verified (spec: added later).
- Ruling (plan vs spec): mixed audio at speed > 1 is already bounded by the existing `AudioOutput` overrun discard (>100 ms queued) with output muted in test mode; Task 8 verifies no audio-driven stall instead of adding a drop path.

## Review Focus

- Client disconnects or crashes mid-hold: the game must release every held key/button immediately (Task 4 test `disconnect_releases_inputs`).
- Slow client: event queue must stay bounded and replies must never be dropped, only `frame` events (Task 4 test `outgoing_queue_drops_frames_not_replies`).
- Speed change while threads sleep: virtual time stays monotonic and sleeping guest threads wake at the scaled time (Task 1 test `speed_change_is_monotonic`, `idle_wait_scales`).
- Game exits or fatals while the client waits: the client fails the step at once with artifacts instead of waiting for the timeout (Task 5 test `wait_aborts_on_exit`).
- Second client or stale pipe from a previous run: refused/rejected cleanly, no hang (Task 4 test `second_client_refused`; Task 5 connect timeout test).

---

### Task 1: Virtual clock

**Files:**
- Modify: `runtime/clock.h`, `runtime/clock.cpp`, `runtime/sched.cpp:259-267`
- Test: `tests/test_clock.cpp`

**Interfaces:**
- Produces: `void uc::set_clock_speed(uint32_t factor)` (1..8, throws `std::invalid_argument` otherwise; rebases so `now_ticks()` stays continuous), `uint32_t uc::clock_speed()`.

- [ ] **Step 1: Write the failing test** — append to `tests/test_clock.cpp` `main` (before `return`):

```cpp
    // Virtual clock: speed scales elapsed time, changes keep it monotonic.
    {
        const auto a=uc::now_ticks(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); const auto b=uc::now_ticks();
        uc::set_clock_speed(4); CHECK(uc::clock_speed()==4);
        const auto c=uc::now_ticks(); CHECK(c>=b);
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); const auto d=uc::now_ticks();
        const double base=double(b-a), fast=double(d-c);
        CHECK(fast>base*3.0 && fast<base*5.5);
        uc::set_clock_speed(1); CHECK(uc::now_ticks()>=d); // speed_change_is_monotonic
        bool rejected=false; try { uc::set_clock_speed(0); } catch(const std::invalid_argument&) { rejected=true; } CHECK(rejected);
        rejected=false; try { uc::set_clock_speed(9); } catch(const std::invalid_argument&) { rejected=true; } CHECK(rejected);
    }
```

Add includes `<chrono>`, `<thread>`, `<stdexcept>` if missing.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmd /c "scripts\env.bat >nul && cmake --build build --target test_clock"`
Expected: compile error `no member named 'set_clock_speed'`.

- [ ] **Step 3: Implement** — `runtime/clock.h` add:

```cpp
void set_clock_speed(uint32_t factor);   // test mode: 1..8, virtual time = base + real elapsed * factor
uint32_t clock_speed();
```

`runtime/clock.cpp` (replace `now_ticks`):

```cpp
#include <atomic>
#include <mutex>
#include <stdexcept>
namespace uc {
namespace {
uint64_t real_ticks() {
    static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return uint64_t(f.QuadPart); }();
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    uint64_t c = uint64_t(q.QuadPart);
    return (c / freq) * kTimebaseHz + (c % freq) * kTimebaseHz / freq;
}
// Seqlock: speed changes are rare, now_ticks is hot.
std::atomic<uint32_t> version{0}, speed{1};
std::atomic<uint64_t> real_base{0}, virtual_base{0};
std::mutex writer;
}
uint64_t now_ticks() {
    for (;;) {
        const uint32_t v = version.load(std::memory_order_acquire);
        if (v & 1) continue;
        const uint64_t r = real_base.load(std::memory_order_relaxed), b = virtual_base.load(std::memory_order_relaxed);
        const uint32_t s = speed.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (version.load(std::memory_order_relaxed) != v) continue;
        const uint64_t now = real_ticks();
        return b + (now - r) * s;
    }
}
void set_clock_speed(uint32_t factor) {
    if (factor < 1 || factor > 8) throw std::invalid_argument("clock speed must be 1..8");
    std::lock_guard lock(writer);
    const uint64_t current = now_ticks(), real = real_ticks();
    version.fetch_add(1, std::memory_order_acq_rel);
    real_base.store(real, std::memory_order_relaxed); virtual_base.store(current, std::memory_order_relaxed); speed.store(factor, std::memory_order_relaxed);
    version.fetch_add(1, std::memory_order_release);
}
uint32_t clock_speed() { return speed.load(std::memory_order_acquire); }
```

Initial state `real_base=virtual_base=0,speed=1` returns plain real time (unchanged behaviour).

`runtime/sched.cpp` `idle_wait`: replace the tick→ms conversion so real waits shrink with speed:

```cpp
        until = std::chrono::steady_clock::now() + std::chrono::milliseconds(t > now ? ticks_to_ms(t - now) / clock_speed() + 1 : 0);
```

- [ ] **Step 4: Add the idle-wait scaling test** (`idle_wait_scales`) to `tests/test_sched.cpp` `main`:

```cpp
    // A guest sleep of 200 virtual ms at speed 4 completes in roughly 50 real ms.
    {
        uc::set_clock_speed(4);
        uc::Sched s(f.c.mem); bool done=false;
        auto* t=s.create(0,16,[&](ppc::Ctx&){ s.sleep_ticks(uc::ms_to_ticks(200)); done=true; });
        const auto start=std::chrono::steady_clock::now(); s.run(t);
        const auto real_ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
        CHECK(done && real_ms<150);
        uc::set_clock_speed(1); s.shutdown(); s.join_hosts();
    }
```

(Use the fixture name already present in `test_sched.cpp`; adapt `f.c.mem` to it.)

- [ ] **Step 5: Run both tests**

Run: `cmd /c "scripts\env.bat >nul && cmake --build build --target test_clock test_sched" && build\tests\test_clock.exe && build\tests\test_sched.exe`
Expected: exit 0 for both.

- [ ] **Step 6: Full suite** — `ctest --test-dir build --output-on-failure` → all pass.

### Task 2: Protocol parser and serializer

**Files:**
- Create: `game/test_protocol.h`, `game/test_protocol.cpp`
- Modify: `game/CMakeLists.txt` (add source to the game library/executable sources list next to `test_control`), `tests/CMakeLists.txt`
- Test: `tests/test_test_protocol.cpp`

**Interfaces:**
- Produces:

```cpp
namespace uc {
enum class TestDevice : uint8_t { kpad, key, mouse, wheel, motion };
struct TestInput { uint64_t at_frame = 0; uint32_t hold_frames = 1; TestDevice device = TestDevice::kpad;
                   float x = 0, y = 0; uint32_t buttons = 0, code = 0; bool down = false; int delta = 0; };
enum class TestRequestKind : uint8_t { input, capture, state, speed, quit };
struct TestRequest { uint64_t id = 0; TestRequestKind kind = TestRequestKind::state; TestInput input; uint32_t factor = 1; };
struct TestParseError { uint64_t id; std::string message; };   // id 0 when unknown
std::variant<TestRequest, TestParseError> parse_test_request(std::string_view line);
std::string test_reply_ok(uint64_t id, std::string_view extra_fields = {});   // extra like "\"path\":\"...\"" (already JSON)
std::string test_reply_error(uint64_t id, std::string_view message);
std::string json_string(std::string_view text);   // quoted, escaped
}
```

- [ ] **Step 1: Write the failing test** `tests/test_test_protocol.cpp`:

```cpp
#include "check.h"
#include "test_protocol.h"
#include <string>
using namespace uc;
static const TestRequest& ok(const std::variant<TestRequest,TestParseError>& v) { CHECK(std::holds_alternative<TestRequest>(v)); return std::get<TestRequest>(v); }
static std::string err(const std::variant<TestRequest,TestParseError>& v) { CHECK(std::holds_alternative<TestParseError>(v)); return std::get<TestParseError>(v).message; }
int main() {
    auto k=ok(parse_test_request(R"({"id":7,"cmd":"input","hold_frames":3,"kpad":{"x":-0.5,"y":0.25,"buttons":2048}})"));
    CHECK(k.id==7 && k.kind==TestRequestKind::input && k.input.device==TestDevice::kpad && k.input.hold_frames==3);
    CHECK(k.input.x==-0.5f && k.input.y==0.25f && k.input.buttons==0x800);
    auto key=ok(parse_test_request(R"({"id":8,"cmd":"input","at_frame":120,"key":{"code":21,"down":true}})"));
    CHECK(key.input.device==TestDevice::key && key.input.code==21 && key.input.down && key.input.at_frame==120);
    CHECK(ok(parse_test_request(R"({"id":9,"cmd":"speed","factor":4})")).factor==4);
    CHECK(ok(parse_test_request(R"({"id":10,"cmd":"capture"})")).kind==TestRequestKind::capture);
    // Rejections change nothing and name the problem.
    CHECK(!err(parse_test_request("not json")).empty());
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"input","kpad":{"x":2,"y":0,"buttons":0}})")).empty());     // out of range
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"input","kpad":{"x":0,"y":0,"buttons":1}})")).empty());     // button outside 0x1C10
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"input","hold_frames":601,"kpad":{"x":0,"y":0,"buttons":0}})")).empty());
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"speed","factor":9})")).empty());
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"state","extra":1})")).empty());                          // unknown field
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"input","key":{"code":0,"down":true}})")).empty());       // key code 1..511
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"input","kpad":{"x":0,"y":0,"buttons":0},"key":{"code":4,"down":true}})")).empty()); // two devices
    CHECK(err(parse_test_request(std::string(4097,' '))).find("long")!=std::string::npos);
    CHECK(std::get<TestParseError>(parse_test_request(R"({"id":5,"cmd":"nope"})")).id==5);              // id kept for the reply
    CHECK(test_reply_ok(3)==R"({"id":3,"ok":true})");
    CHECK(test_reply_error(3,"bad \"x\"")==R"({"id":3,"error":"bad \"x\""})");
    CHECK(test_reply_ok(4,R"("path":"a")")==R"({"id":4,"ok":true,"path":"a"})");
}
```

`tests/CMakeLists.txt`:

```cmake
add_executable(test_test_protocol test_test_protocol.cpp ${CMAKE_SOURCE_DIR}/game/test_protocol.cpp)
target_include_directories(test_test_protocol PRIVATE ${CMAKE_SOURCE_DIR}/game)
target_link_libraries(test_test_protocol PRIVATE test_support)
add_test(NAME test_test_protocol COMMAND test_test_protocol)
```

- [ ] **Step 2: Run to verify it fails** — `cmake --build build --target test_test_protocol` → missing header.

- [ ] **Step 3: Implement** `game/test_protocol.cpp`: a minimal JSON reader for this fixed shape — object of string keys; values: string, number (`std::from_chars` double), `true/false`, nested object (depth ≤ 2). Reject anything else (arrays, null, nesting > 2, trailing bytes, duplicate keys).

```cpp
#include "test_protocol.h"
#include <charconv>
#include <cmath>
#include <map>
#include <memory>
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
}
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
        if (name == "capture" || name == "state" || name == "quit") {
            only(root, {"id", "cmd"});
            r.kind = name == "capture" ? TestRequestKind::capture : name == "state" ? TestRequestKind::state : TestRequestKind::quit;
        } else if (name == "speed") {
            only(root, {"id", "cmd", "factor"}); r.kind = TestRequestKind::speed; r.factor = uint32_t(number(root, "factor", 1, 8, true));
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
}
```

Note: a malformed line can still yield `id` when the object parsed far enough; the server replies with it (id 0 otherwise as an `{"error":...}` event).

- [ ] **Step 4: Run** — build + `build\tests\test_test_protocol.exe` → exit 0.

### Task 3: Game state publication

**Files:**
- Create: `game/test_state.h`, `game/test_state.cpp`
- Modify: `game/game_input.cpp` (`game_input_sample` after `const auto context=...`; `action_log`), `game/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/test_test_state.cpp`

**Interfaces:**
- Consumes: `uc::InputProbeSnapshot` (`game/input_probe.h`), `uc::InputContext` (`game/game_input.h`: inactive=0, menu=1, gameplay=2, qte=3).
- Produces:

```cpp
namespace uc {
struct TestFrameState {
    uint64_t frame = 0;                       // KPAD samples since start
    uint8_t context = 0;                      // InputContext value
    std::array<uint32_t,12> ui{}; bool ui_valid = false;
    uint32_t pause = 0, scene_mode = 0, tutorial = 0; bool tutorial_loaded = false;
    bool player_valid = false; uint32_t player_mode = 0, player_state = 0;
};
struct TestAction { std::string name; bool accepted; uint64_t frame; };
void test_state_enable(bool);                                   // before guest threads start
void test_state_publish(const InputProbeSnapshot&, uint8_t context); // guest thread, once per KPAD sample
TestFrameState test_state_latest();                             // any thread
void test_state_action(const char* name, bool accepted);        // guest thread
std::vector<TestAction> test_state_take_actions();              // main thread; bounded 256, oldest dropped
std::string test_frame_json(const TestFrameState&, uint32_t speed, double render_ms, uint64_t presents, uint64_t retraces, uint64_t dropped);
}
```

- [ ] **Step 1: Failing test** `tests/test_test_state.cpp`:

```cpp
#include "check.h"
#include "test_state.h"
#include "input_probe.h"
int main() {
    uc::InputProbeSnapshot s; s.pause_state=2; s.game_mode=3; s.tutorial_loaded=true; s.tutorial_state=0x00100000;
    s.ui_states[6]=5; s.ui_valid=true; s.players[0].valid=true; s.players[0].mode_630=1; s.players[0].state_794=1;
    uc::test_state_publish(s,2); CHECK(uc::test_state_latest().frame==0);   // disabled: nothing published
    uc::test_state_enable(true);
    uc::test_state_publish(s,2); uc::test_state_publish(s,1);
    auto f=uc::test_state_latest();
    CHECK(f.frame==2 && f.context==1 && f.ui[6]==5 && f.pause==2 && f.scene_mode==3 && f.tutorial==0x00100000 && f.player_mode==1);
    for(int i=0;i<300;++i) uc::test_state_action("Reload",true);
    auto actions=uc::test_state_take_actions(); CHECK(actions.size()==256 && actions.back().name=="Reload" && actions.back().frame==2);
    CHECK(uc::test_state_take_actions().empty());
    const auto json=uc::test_frame_json(f,4,2.5,30,60,0);
    CHECK(json.find(R"("type":"frame")")!=std::string::npos && json.find(R"("n":2)")!=std::string::npos);
    CHECK(json.find(R"("context":"menu")")!=std::string::npos && json.find(R"("speed":4)")!=std::string::npos);
    CHECK(json.find(R"("ui":[0,0,0,0,0,0,5,0,0,0,0,0])")!=std::string::npos);
}
```

CMake entry like Task 2 with `${CMAKE_SOURCE_DIR}/game/test_state.cpp`, include dir `game`.

- [ ] **Step 2: Run to verify it fails** (missing header).

- [ ] **Step 3: Implement** `game/test_state.cpp`:

```cpp
#include "test_state.h"
#include "input_probe.h"
#include <atomic>
#include <deque>
#include <mutex>
namespace uc {
namespace {
std::atomic<bool> enabled{false};
std::mutex lock;
TestFrameState latest;
std::deque<TestAction> actions;
}
void test_state_enable(bool on) { enabled = on; }
void test_state_publish(const InputProbeSnapshot& s, uint8_t context) {
    if (!enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard g(lock);
    ++latest.frame; latest.context = context; latest.ui = s.ui_states; latest.ui_valid = s.ui_valid;
    latest.pause = s.pause_state; latest.scene_mode = s.game_mode; latest.tutorial = s.tutorial_state; latest.tutorial_loaded = s.tutorial_loaded;
    latest.player_valid = s.players[0].valid; latest.player_mode = s.players[0].mode_630; latest.player_state = s.players[0].state_794;
}
TestFrameState test_state_latest() { std::lock_guard g(lock); return latest; }
void test_state_action(const char* name, bool accepted) {
    if (!enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard g(lock);
    if (actions.size() == 256) actions.pop_front();
    actions.push_back({name, accepted, latest.frame});
}
std::vector<TestAction> test_state_take_actions() { std::lock_guard g(lock); std::vector<TestAction> out(actions.begin(), actions.end()); actions.clear(); return out; }
std::string test_frame_json(const TestFrameState& f, uint32_t speed, double render_ms, uint64_t presents, uint64_t retraces, uint64_t dropped) {
    static const char* names[] = {"inactive", "menu", "gameplay", "qte"};
    std::string ui = "[";
    for (size_t i = 0; i < f.ui.size(); ++i) ui += (i ? "," : "") + std::to_string(int32_t(f.ui[i]));
    ui += "]";
    char buffer[768];
    std::snprintf(buffer, sizeof buffer,
        R"({"type":"frame","n":%llu,"speed":%u,"context":"%s","ui":%s,"ui_valid":%s,"pause":%u,"scene_mode":%u,"tutorial":%u,"tutorial_loaded":%s,"player":{"valid":%s,"mode":%u,"state":%u},"render_ms":%.3f,"presents":%llu,"retraces":%llu,"dropped":%llu})",
        (unsigned long long)f.frame, speed, names[f.context & 3], ui.c_str(), f.ui_valid ? "true" : "false", f.pause, f.scene_mode, f.tutorial,
        f.tutorial_loaded ? "true" : "false", f.player_valid ? "true" : "false", f.player_mode, f.player_state, render_ms,
        (unsigned long long)presents, (unsigned long long)retraces, (unsigned long long)dropped);
    return buffer;
}
}
```

UI states are printed as signed (`-1` = inactive slot, matching `game_input_context`'s `int32_t(state)>=0` test).

Hook `game/game_input.cpp`: in `game_input_sample` right after `const auto context=game_input_context(state);` add `uc::test_state_publish(state,uint8_t(context));`; in `action_log` first line add `uc::test_state_action(action,accepted);`. Include `"test_state.h"`.

- [ ] **Step 4: Run** test → exit 0; full `ctest` passes.

### Task 4: Test server (pipes, frame-scheduled commands, main-loop integration)

**Files:**
- Create: `game/test_pipe.h`, `game/test_pipe.cpp` (Win32 transport), `game/test_server.h`, `game/test_server.cpp` (command scheduling, replies)
- Modify: `game/options.h`, `game/options.cpp` (`--test-server <name>`, `--speed <1..8>`, `--capture-dir <dir>`), `game/uc_main.cpp` (setup, per-iteration pump, capture path, quit, disconnect release), `game/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/test_options.cpp`
- Test: `tests/test_test_server.cpp`

**Interfaces:**
- Consumes: Task 1 `set_clock_speed`, Task 2 `parse_test_request`/replies, Task 3 `test_state_latest`/`test_state_take_actions`/`test_frame_json`.
- Produces:

```cpp
namespace uc {
class TestPipes {                         // two pipes: \\.\pipe\uc-test-<name>-cmd (in), -evt (out)
public:
    explicit TestPipes(const std::string& name);   // creates both pipes, starts reader/writer threads
    ~TestPipes();                                    // cancels blocking I/O, joins
    std::vector<std::string> take_lines();           // complete command lines received since last call
    void send(std::string line, bool droppable);     // droppable=frame event; queue ≤ 4096, drops oldest droppable
    bool connected() const; bool disconnected() const; uint64_t dropped() const;
};
struct TestInputSink {                     // implemented over NativeInput in uc_main, a fake in tests
    virtual ~TestInputSink() = default;
    virtual bool kpad(uint64_t sequence, float x, float y, uint32_t buttons) = 0;
    virtual bool host(uint64_t sequence, const HostEvent&) = 0;
    virtual uint64_t consumed() = 0;
};
class TestServer {
public:
    TestServer(TestPipes&, TestInputSink&);
    // Called once per main-loop iteration with the latest published frame counter.
    struct Pump { bool capture = false; bool quit = false; };
    Pump pump(uint64_t frame, const std::string& frame_json);
    void captured(const std::string& path);           // completes the oldest pending capture
    void capture_failed(const std::string& message);
    void release_all();                               // on disconnect/exit
};
}
```

Scheduling: each accepted `input` becomes items `{due_frame, press}` and, for kpad/key/mouse, `{due_frame+hold_frames, release}`; `due_frame = max(at_frame, frame+1)`. Items apply when `frame >= due_frame`, each with a fresh internal sequence. The reply `{"id":n,"ok":true,"consumed_frame":f}` is sent when `sink.consumed()` reaches the sequence of the request's last item. At most 256 pending requests; more → error reply. kpad release keeps the aim and clears buttons; key/mouse release sends `down=false`.

- [ ] **Step 1: Failing test** `tests/test_test_server.cpp` — scheduling with a fake sink, plus a real pipe round trip:

```cpp
#include "check.h"
#include "test_server.h"
#include "test_pipe.h"
#include <windows.h>
#include <string>
#include <thread>
struct FakeSink : uc::TestInputSink {
    std::vector<std::tuple<uint64_t,float,float,uint32_t>> kpads; std::vector<uc::HostEvent> hosts; uint64_t consumed_seq=0, last=0;
    bool kpad(uint64_t s,float x,float y,uint32_t b) override { kpads.emplace_back(s,x,y,b); last=s; return true; }
    bool host(uint64_t s,const uc::HostEvent& e) override { hosts.push_back(e); last=s; return true; }
    uint64_t consumed() override { return consumed_seq; }
};
static HANDLE open_pipe(const std::wstring& name,DWORD access) {
    for(int i=0;i<200;++i){ HANDLE h=CreateFileW(name.c_str(),access,0,nullptr,OPEN_EXISTING,0,nullptr); if(h!=INVALID_HANDLE_VALUE) return h; Sleep(10);} return INVALID_HANDLE_VALUE;
}
static void write_line(HANDLE h,const std::string& s){ std::string l=s+"\n"; DWORD n=0; CHECK(WriteFile(h,l.data(),DWORD(l.size()),&n,nullptr) && n==l.size()); }
static std::string read_line(HANDLE h){ std::string out; char c; DWORD n=0; while(ReadFile(h,&c,1,&n,nullptr) && n==1 && c!='\n') out+=c; return out; }
int main() {
    const std::string name="unit-"+std::to_string(GetCurrentProcessId());
    uc::TestPipes pipes(name); FakeSink sink; uc::TestServer server(pipes,sink);
    HANDLE cmd=open_pipe(L"\\\\.\\pipe\\uc-test-"+std::wstring(name.begin(),name.end())+L"-cmd",GENERIC_WRITE);
    HANDLE evt=open_pipe(L"\\\\.\\pipe\\uc-test-"+std::wstring(name.begin(),name.end())+L"-evt",GENERIC_READ);
    CHECK(cmd!=INVALID_HANDLE_VALUE && evt!=INVALID_HANDLE_VALUE);
    // second_client_refused: the single instance is taken.
    CHECK(CreateFileW((L"\\\\.\\pipe\\uc-test-"+std::wstring(name.begin(),name.end())+L"-cmd").c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr)==INVALID_HANDLE_VALUE);
    write_line(cmd,R"({"id":1,"cmd":"input","hold_frames":2,"kpad":{"x":0.5,"y":-0.5,"buttons":2048}})");
    for(int i=0;i<100 && sink.kpads.empty();++i){ server.pump(10,"{\"type\":\"frame\",\"n\":10}"); Sleep(5); }
    CHECK(sink.kpads.empty());                                   // due at frame 11, not 10
    server.pump(11,"{}"); CHECK(sink.kpads.size()==1 && std::get<3>(sink.kpads[0])==0x800);
    server.pump(12,"{}"); CHECK(sink.kpads.size()==1);
    server.pump(13,"{}"); CHECK(sink.kpads.size()==2 && std::get<3>(sink.kpads[1])==0 && std::get<1>(sink.kpads[1])==0.5f);
    sink.consumed_seq=sink.last; server.pump(14,"{\"type\":\"frame\",\"n\":14}");
    std::string reply; for(int i=0;i<20 && reply.find("\"id\":1")==std::string::npos;++i) reply=read_line(evt);
    CHECK(reply.find(R"("ok":true)")!=std::string::npos && reply.find(R"("consumed_frame":14)")!=std::string::npos);
    write_line(cmd,R"({"id":2,"cmd":"input","kpad":{"x":3,"y":0,"buttons":0}})");
    server.pump(15,"{}"); for(int i=0;i<20 && reply.find("\"id\":2")==std::string::npos;++i){ server.pump(15,"{}"); reply=read_line(evt); }
    CHECK(reply.find("error")!=std::string::npos);
    // outgoing_queue_drops_frames_not_replies
    for(int i=0;i<10000;++i) pipes.send("{\"type\":\"frame\"}",true);
    pipes.send(R"({"id":99,"ok":true})",false);
    bool got=false; for(int i=0;i<20000 && !got;++i) got=read_line(evt).find("\"id\":99")!=std::string::npos;
    CHECK(got && pipes.dropped()>0);
    // disconnect_releases_inputs: key held, client closes, server releases.
    write_line(cmd,R"({"id":3,"cmd":"input","hold_frames":100,"key":{"code":21,"down":true}})");
    for(int i=0;i<50 && sink.hosts.empty();++i){ server.pump(20,"{}"); Sleep(5); }
    CHECK(sink.hosts.size()==1 && sink.hosts[0].value==1);
    CloseHandle(cmd); CloseHandle(evt);
    for(int i=0;i<100 && !pipes.disconnected();++i) Sleep(5);
    server.release_all(); CHECK(sink.hosts.size()==2 && sink.hosts[1].value==0 && sink.hosts[1].code==21);
}
```

CMake: `add_executable(test_test_server test_test_server.cpp ${CMAKE_SOURCE_DIR}/game/test_server.cpp ${CMAKE_SOURCE_DIR}/game/test_pipe.cpp ${CMAKE_SOURCE_DIR}/game/test_protocol.cpp)`, include `game` and `runtime`, link `test_support`, `add_test`.

- [ ] **Step 2: Run to verify it fails** (missing headers).

- [ ] **Step 3: Implement `game/test_pipe.cpp`**:
  - `CreateNamedPipeW(L"\\\\.\\pipe\\uc-test-"+name+L"-cmd", PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 65536, 0, nullptr)`; `-evt` with `PIPE_ACCESS_OUTBOUND`. Failure (name in use) throws `std::runtime_error("test pipe already in use: "+name)`.
  - Reader thread: `ConnectNamedPipe` (treat `ERROR_PIPE_CONNECTED` as success), then `ReadFile` into a 4096-byte buffer, split on `\n`, strip `\r`; a line > 4096 bytes is discarded and replaced by the marker line `"\x01too-long"` which the server answers with `{"error":"command line too long"}`. Lines go to a mutex-protected vector. `ReadFile` failure → `disconnected=true`, thread ends.
  - Writer thread: `ConnectNamedPipe`, then waits on a condition variable for the outgoing deque and `WriteFile`s each line + `\n`; failure → `disconnected=true`.
  - `send`: if the deque holds 4096 lines, erase the oldest droppable line and count `dropped`; if none is droppable, the new line is kept anyway (replies are never dropped).
  - Destructor: set stop, `CancelSynchronousIo` on both thread handles (unblocks Connect/Read/Write), notify cv, join, close handles.
- [ ] **Step 4: Implement `game/test_server.cpp`** per the interface: parse each line (`parse_test_request`; marker → error), dispatch `state` (reply `test_reply_ok(id,"\"state\":"+frame_json)`), `speed` (`set_clock_speed`, reply with `"factor":k`), `capture` (queue id, set `Pump.capture`), `quit` (reply, `Pump.quit`), `input` (schedule as described). Then apply due items in order, send replies whose last sequence is consumed, send `action` events from `test_state_take_actions()` (`{"type":"action","name":…,"accepted":…,"frame":…}`) and finally `pipes.send(frame_json,true)` when `frame` advanced since the last pump. `release_all` sends `down=false` for every held key/mouse button and a kpad release (buttons 0) when kpad buttons are held, then clears the schedule.
- [ ] **Step 5: Run** `test_test_server` → exit 0.
- [ ] **Step 6: Options** — `game/options.h` add `std::string test_server; uint32_t speed=1; std::filesystem::path capture_dir;` parse in `options.cpp` (`--test-server <name>` name `[A-Za-z0-9_-]{1,32}`, `--speed <1..8>` requires `--test-server`, `--capture-dir <dir>`). Add `tests/test_options.cpp` cases: valid server/speed parse; `--speed 9`, `--speed 2` without server, `--test-server bad/name` rejected.
- [ ] **Step 7: Integrate in `game/uc_main.cpp`**:
  - `--test-server` implies `controlled` (same isolation checks as `--test-control`; both together → error) and calls `uc::test_state_enable(true)` before `GuestRunner` starts; `set_clock_speed(options.speed)` before `vi_hle_init`.
  - `NativeInputSink` adapter: `kpad` → `input.apply_test_command(seq,x,y,buttons)`; `host` → `input.apply_test_event(seq,event,width,height)` with the current window size; `consumed` → `input.last_consumed_sequence()`.
  - In the main loop where `--test-control` is polled, when the server exists: `auto f=uc::test_state_latest(); auto pump=server->pump(f.frame,uc::test_frame_json(f,uc::clock_speed(),last_render_ms,renderer.presentation_count(),uc::vi_stats().retraces,pipes->dropped())); debug_capture_requested|=pump.capture; close|=pump.quit;`
  - Capture: where `debug screenshot:` is printed, use `options.capture_dir` when set (else the existing directory) and call `server->captured(path.string())`; on failure `server->capture_failed(message)`.
  - Each iteration, if `pipes->disconnected()` and inputs are held, call `server->release_all()` once.
  - On exit send `{"type":"exit","code":0}`; `crash_report_write` callers already print the path — add `test_server_fatal(reason)` hook in `fatal.cpp`? No: keep the pipe free of crash paths; the client detects the dead process and reads `debug/crash-*.txt` itself (Task 5).
- [ ] **Step 8: Full suite + live smoke**: `build.bat --no-install`; then in PowerShell:

```powershell
$d=New-Item -ItemType Directory -Force build\autotest\server-smoke; Copy-Item "<install dir>\saves" "$d\saves" -Recurse -Force
Start-Process dist\uc.exe -ArgumentList "--skip-intro","--seconds","30","--test-server","smoke","--save-root","`"$d\saves`"","--capture-dir","`"$d`""
python -c "import time,json;time.sleep(3);c=open(r'\\.\pipe\uc-test-smoke-cmd','wb',buffering=0);e=open(r'\\.\pipe\uc-test-smoke-evt','rb');c.write(b'{\"id\":1,\"cmd\":\"capture\"}\n');[print(e.readline()) for _ in range(400) ]" | Select-String '"id":1|"type":"frame"' | Select-Object -First 3
```

Expected: `frame` lines with increasing `n` and a reply `{"id":1,"ok":true,"path":"...png"}`.

### Task 5: Python client core

**Files:**
- Create: `scripts/uctest/__init__.py`, `scripts/uctest/transport.py`, `scripts/uctest/client.py`
- Test: `tests/test_uctest_client.py`; Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: protocol of Tasks 2–4.
- Produces:

```python
class StepFailed(Exception):
    def __init__(self, message: str, artifacts: dict): ...   # artifacts: frame, capture, log_tail, crash_report, awaited
class PipeTransport:            # transport.py
    def __init__(self, name: str, connect_seconds: float = 20.0): ...
    def send_line(self, line: str) -> None: ...
    def read_line(self) -> str | None: ...   # None on EOF
    def close(self) -> None: ...
class Game:                     # client.py
    BUTTONS = {"A": 0x0800, "B": 0x0400, "minus": 0x0010, "plus": 0x1000}
    def __init__(self, *, exe="dist/uc.exe", run_dir=None, speed=1, skip_intro=True, seconds=600, transport_factory=None, launch=True): ...
    def __enter__(self) -> "Game": ...; def __exit__(self, *exc) -> None: ...   # always terminates the process
    frame: dict                                    # latest frame event
    def request(self, cmd: dict, seconds: float = 30) -> dict: ...
    def wait(self, predicate, *, frames: int | None = None, seconds: float = 60, awaited: str = "") -> dict: ...
    def press(self, button: str, frames: int = 2, x: float = 0.0, y: float = 0.0) -> int: ...   # returns consumed_frame
    def aim(self, x: float, y: float, settle_frames: int = 0) -> int: ...
    def key(self, code: int, frames: int = 2) -> int: ...
    def capture(self) -> str: ...
    def state(self) -> dict: ...
    def set_speed(self, factor: int) -> None: ...
    def actions(self) -> list[dict]: ...          # drains received action events
    def fail(self, message: str, awaited: str = "") -> "NoReturn": ...
```

- [ ] **Step 1: Failing test** `tests/test_uctest_client.py` (fake transport, no game):

```python
import queue, sys, threading, time, unittest
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / "scripts"))
from uctest.client import Game, StepFailed

class FakeTransport:
    def __init__(self):
        self.out = queue.Queue(); self.sent = []; self.closed = False
    def send_line(self, line):
        self.sent.append(line)
        import json; cmd = json.loads(line)
        if cmd["cmd"] == "input": self.out.put(json.dumps({"id": cmd["id"], "ok": True, "consumed_frame": 12}))
        elif cmd["cmd"] == "capture": self.out.put(json.dumps({"id": cmd["id"], "ok": True, "path": "x.png"}))
        elif cmd["cmd"] == "speed": self.out.put(json.dumps({"id": cmd["id"], "ok": True, "factor": cmd["factor"]}))
    def read_line(self):
        line = self.out.get(); return line
    def close(self): self.closed = True; self.out.put(None)
    def frame(self, n, context="menu", **extra):
        import json; self.out.put(json.dumps({"type": "frame", "n": n, "context": context, "speed": 1, "ui": [-1]*12, **extra}))

class ClientTest(unittest.TestCase):
    def game(self, t):
        return Game(transport_factory=lambda: t, launch=False, run_dir=Path(self.tmp.name))
    def setUp(self):
        import tempfile; self.tmp = tempfile.TemporaryDirectory()
    def tearDown(self): self.tmp.cleanup()
    def test_press_waits_for_consumption(self):
        t = FakeTransport()
        with self.game(t) as g:
            self.assertEqual(g.press("A"), 12)
            self.assertIn('"buttons": 2048', t.sent[-1])
    def test_wait_on_state(self):
        t = FakeTransport()
        with self.game(t) as g:
            threading.Timer(0.05, lambda: [t.frame(1), t.frame(2, "gameplay")]).start()
            self.assertEqual(g.wait(lambda f: f["context"] == "gameplay", seconds=2)["n"], 2)
    def test_wait_frame_budget(self):
        t = FakeTransport()
        with self.game(t) as g:
            for n in range(1, 40): t.frame(n)
            with self.assertRaises(StepFailed) as e: g.wait(lambda f: f["context"] == "gameplay", frames=10, seconds=2, awaited="gameplay")
            self.assertEqual(e.exception.artifacts["awaited"], "gameplay")
    def test_wait_aborts_on_exit(self):
        t = FakeTransport()
        with self.game(t) as g:
            start = time.monotonic(); threading.Timer(0.05, t.close).start()
            with self.assertRaises(StepFailed): g.wait(lambda f: False, seconds=30)
            self.assertLess(time.monotonic() - start, 5)
    def test_error_reply_raises(self):
        t = FakeTransport(); t.send_line = lambda line: t.out.put('{"id":1,"error":"bad"}')
        with self.game(t) as g:
            with self.assertRaises(StepFailed): g.request({"cmd": "state"})

if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
```

CMake: `add_test(NAME test_uctest_client COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test_uctest_client.py ${CMAKE_SOURCE_DIR})`.

- [ ] **Step 2: Run to verify it fails** — `python -I tests/test_uctest_client.py .` → `ModuleNotFoundError: uctest`.
- [ ] **Step 3: Implement** `transport.py`: `PipeTransport` opens `\\.\pipe\uc-test-{name}-cmd` (`open(..., "wb", buffering=0)`) then `-evt` (`open(..., "rb")`), retrying `FileNotFoundError`/`PermissionError` every 50 ms until `connect_seconds`, then raises `TimeoutError("test pipe uc-test-<name> not available")`. `read_line` returns decoded line without `\n`, `None` at EOF/`OSError`.

`client.py` essentials:
- `__init__`: `run_dir` default `build/autotest/<time_ns>`; when `launch`, copy `<install dir>\saves` to `run_dir/saves`, start `exe` with `--test-server <run name> --save-root <saves> --capture-dir <run_dir> --seconds <seconds> --speed <speed>` (+ `--skip-intro`), stdout/stderr to `run_dir/run.log`, `creationflags=0` (visible console as before), then `transport_factory()` default `PipeTransport(name)`.
- Reader thread: `json.loads` each line; `type=="frame"` → `self.frame`, append to `deque(maxlen=600)`, notify condition; `type=="action"` → `self._actions.append`; `type=="exit"` → `self.exited=True`; dict with `id` → `self._replies[id]=obj`, notify. EOF → `self.exited=True`, notify.
- `request`: assign next id, send `json.dumps(cmd|{"id":id})`, wait on condition until reply, exit or `seconds`; `error` → `fail(...)`.
- `wait`: start `n = self.frame.get("n",0)`; loop: predicate(frame) true → return frame; `frames` budget exceeded (`frame.n - n > frames`) or `seconds` elapsed or `exited` → `fail`.
- `fail`: artifacts = `{"awaited", "frame", "log_tail": last 80 lines of run.log, "crash_report": newest debug/crash-*.txt next to exe if newer than start, "capture": try self.capture() with 5 s timeout unless exited}`; write `run_dir/failure.json`; raise `StepFailed`.
- `__exit__`: try `quit` (2 s), close transport, `process.terminate()`, `wait(5)`, then `kill()`; never raises from cleanup.
- [ ] **Step 4: Run** test → `OK`; full ctest passes.

### Task 6: Menu map, high-level steps, scenarios, CLI

**Files:**
- Create: `scripts/uctest/menus.py`, `scripts/uctest/steps.py`, `scripts/uctest/scenarios.py`, `scripts/uctest/__main__.py`
- Test: `tests/test_uctest_steps.py`; Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 5 `Game`.
- Produces:

```python
# menus.py
@dataclass(frozen=True)
class Screen: name: str; signature: dict[int, int]      # ui slot -> value that identifies the screen
@dataclass(frozen=True)
class Target: screen: str; x: float; y: float
SCREENS: dict[str, Screen]; TARGETS: dict[str, Target]
def screen_of(frame: dict) -> str | None
# steps.py
def skip_movies(g, *, seconds=30) -> None
def select(g, target: str, *, retries=2) -> None
def wait_gameplay(g, *, seconds=60) -> dict
def perform(g, action: str, *, seconds=10) -> dict          # fire|reload|grenade|knife|pause
def measure_fps(g, seconds: float) -> dict                  # {"fps_mean","fps_min","samples"}
# scenarios.py
SCENARIOS: dict[str, Callable[[Game, argparse.Namespace], dict]]   # boot_to_title, train_gameplay, controls, perf
```

- [ ] **Step 1: Discover screen signatures (live, measured data).** Run the old route once with the server to log UI slots per screen:

```powershell
python -c "import sys;sys.path.insert(0,'scripts');from uctest.client import Game
with Game(seconds=200) as g:
  seen=set()
  coords=[(0,0),(0,0),(-0.425,-0.05),(-0.5125,-0.0333),(-0.325,-0.2),(-0.24,-0.067),(0.625,0.4),(-0.35,0.2),(0,0)]
  import time
  for x,y in coords:
    t=time.monotonic()
    while time.monotonic()-t<4:
      f=g.frame; k=(f['context'],tuple(f['ui']));
      if k not in seen: seen.add(k); print(f['n'],k)
      time.sleep(0.05)
    g.aim(x,y,settle_frames=10); g.press('A',x=x,y=y)"
```

Record each distinct `ui` tuple that appears after each tap; write them into `SCREENS` (e.g. `Screen("save_select",{6:<value>,4:<value>})` using only slots that differ between screens) and the existing coordinates into `TARGETS`: `save_slot_1(-0.425,-0.05)`, `scenario_train(-0.5125,-0.0333)`, `chapter_1(-0.325,-0.2)`, `character_rebecca(-0.24,-0.067)`, `loadout_confirm(0.625,0.4)`, `start_yes(-0.35,0.2)`. Commit the measured values in `menus.py` with a comment naming the run directory.

- [ ] **Step 2: Failing test** `tests/test_uctest_steps.py` — scripted fake game:

```python
import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / "scripts"))
from uctest import steps, menus
from uctest.client import StepFailed

class ScriptedGame:
    """Frames advance on every press; screens change according to a script."""
    def __init__(self, screens):
        self.screens = list(screens); self.presses = []; self.n = 0
        self.frame = self._frame()
    def _frame(self):
        sig = menus.SCREENS[self.screens[0]].signature if self.screens else {}
        ui = [-1] * 12
        for slot, value in sig.items(): ui[slot] = value
        return {"n": self.n, "context": "menu", "ui": ui, "speed": 1}
    def aim(self, x, y, settle_frames=0): self.n += settle_frames; self.frame = self._frame(); return self.n
    def press(self, button, frames=2, x=0.0, y=0.0):
        self.presses.append((button, x, y)); self.n += frames
        if len(self.screens) > 1 and not (len(self.presses) == 1 and getattr(self, "drop_first", False)): self.screens.pop(0)
        self.frame = self._frame(); return self.n
    def wait(self, predicate, frames=None, seconds=60, awaited=""):
        for _ in range(3):
            if predicate(self.frame): return self.frame
            self.n += 1; self.frame = self._frame()
        raise StepFailed("timeout " + awaited, {"awaited": awaited})
    def fail(self, message, awaited=""): raise StepFailed(message, {"awaited": awaited})

class StepsTest(unittest.TestCase):
    def test_select_presses_target_and_sees_change(self):
        t = menus.TARGETS["save_slot_1"]; g = ScriptedGame([t.screen, "scenario"])
        steps.select(g, "save_slot_1")
        self.assertEqual(g.presses, [("A", t.x, t.y)])
    def test_select_retries_when_screen_does_not_change(self):
        t = menus.TARGETS["save_slot_1"]; g = ScriptedGame([t.screen, "scenario"]); g.drop_first = True
        steps.select(g, "save_slot_1")
        self.assertEqual(len(g.presses), 2)
    def test_select_wrong_screen_fails(self):
        g = ScriptedGame(["scenario"])
        with self.assertRaises(StepFailed): steps.select(g, "save_slot_1")
    def test_measure_rejects_speed(self):
        g = ScriptedGame(["scenario"]); g.frame["speed"] = 4
        with self.assertRaises(StepFailed): steps.measure_fps(g, 1)

if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
```

(The screen named `"scenario"` is the screen reached from `save_slot_1` in the measured map; use the actual names chosen in Step 1.)

- [ ] **Step 3: Run to verify it fails** (`ImportError: steps`).
- [ ] **Step 4: Implement** `menus.py` (data from Step 1; `screen_of` returns the first screen whose signature matches), `steps.py`:
  - `select`: `screen = TARGETS[t].screen`; `g.wait(lambda f: screen_of(f)==screen, frames=600, awaited=screen)` else fail; for attempt in range(retries+1): `g.aim(x,y,settle_frames=10)`; `g.press("A",2,x,y)`; `try: g.wait(lambda f: screen_of(f)!=screen, frames=120, awaited=f"leave {screen}"); return` except `StepFailed`: continue; finally `g.fail(f"{t} had no effect after {retries+1} presses")`.
  - `skip_movies`: while `g.frame["context"]=="inactive"` within `seconds`: `g.press("A")`; then `g.wait(lambda f: f["context"]!="inactive", frames=300)`.
  - `wait_gameplay`: `g.wait(lambda f: f["context"]=="gameplay", seconds=seconds, awaited="gameplay")`, dismissing tutorial pages by pressing A while `context=="menu"` and `tutorial_loaded` and `tutorial!=0x00100000`.
  - `perform`: maps action → input (fire: `press("B")`; reload/grenade/knife/pause: PC key codes (SDL scancodes) of `default_bindings()` in `common/input_bindings.cpp` via `g.key(code)`; copy the reload/grenade/knife/pause scancodes from that function into `steps.py` constants with a comment naming the source), then waits for an `action` event with matching name and `accepted` within `seconds`.
  - `measure_fps`: fail unless `g.frame["speed"]==1`; sample `presents` deltas per second for `seconds` while `context=="gameplay"`; discard seconds containing a context change.
  - `scenarios.py`: `boot_to_title` (skip_movies, wait title screen, capture); `train_gameplay` (boot, `select` the six targets in order, skip mission movie, `wait_gameplay`, capture); `controls` (train_gameplay then `perform` each action, report accepted list); `perf` (train_gameplay, then `measure_fps(args.measure or 15)`).
  - `__main__.py`: `python -m uctest run <scenario> [--speed k] [--measure s] [--repeat n]`; per repetition a `Game`, catches `StepFailed`, writes `report.json` `{scenario, ok, seconds, steps:[{name,seconds}], artifacts}`, prints one summary line per run; exit 0 only if all repetitions pass. `python -m uctest list` prints scenario names.
- [ ] **Step 5: Run** `test_uctest_steps` and `test_uctest_client` → OK; full ctest passes.
- [ ] **Step 6: Live** — `python -m uctest run train_gameplay` (run from `scripts` with `PYTHONPATH`, or `cd scripts`): exit 0, capture shows gameplay; `controls` lists all actions accepted.

### Task 7: Hovered menu item in RAM

**Files:**
- Modify (if found): `game/input_probe.h/.cpp` (new field `menu_hover`), `game/test_state.*` (`hover` in frame JSON), `scripts/uctest/steps.py` (`select` checks hover before pressing), `tests/test_test_state.cpp`
- Doc: `docs/testing/test-server.md` (result either way)

- [ ] **Step 1:** With the server running on the save menu, aim at slot 1, then slot 2 (`aim`, 20 frames each), and diff guest RAM snapshots of the UI object whose state is in `ui_states` (the probe already reads its address; dump 0x400 bytes around it via a temporary `state` extension or the existing `--input-probe` record output). A field that changes between exactly the two hover positions and back is the candidate.
- [ ] **Step 2:** Confirm on a second menu (chapter select) that the same field tracks the hovered item.
- [ ] **Step 3 (found):** add `menu_hover` to `InputProbeSnapshot` and frame JSON with a unit test asserting serialization; `select` waits for `hover==expected` before pressing (expected indices recorded in `TARGETS`). **(not found):** record the searched objects and offsets in `docs/testing/test-server.md` under "Known gaps"; `select` keeps the screen-change check.

### Task 8: Acceleration verification, reliability, documentation

**Files:**
- Create: `.claude/skills/uc-test/SKILL.md`, `docs/testing/test-server.md`, `AGENTS.md`
- Modify: `docs/spikes/G14-internal-test-control.md` (superseded note), `scripts/train_route.py` (module docstring: superseded by `python -m uctest`), `docs/tech-debt.md`

- [ ] **Step 1: Speed check** — run `train_gameplay` at `--speed 1`, `2`, `4`, `8`; record wall time and success. For each failure, identify the subsystem (movie, audio startup wait, alarms) from `failure.json`; reduce the scenario's speed for that step (`g.set_speed(1)` around it) and add a TD row naming the subsystem. Record achieved speed (retraces per real second ÷ 60) — if presentation caps it, add the render-skip TD row.
- [ ] **Step 2: Reliability** — `python -m uctest run train_gameplay --repeat 10` at the fastest stable speed and at speed 1: 10/10 must pass; compare total time with `scripts/train_route.py` (≈80 s per run). Any failure → fix root cause (systematic-debugging) and repeat until 10/10.
- [ ] **Step 3: `docs/testing/test-server.md`** — protocol reference (commands, replies, events with every field, limits, pipe names), launch flags, how frames are counted, speed semantics, known gaps.
- [ ] **Step 4: `.claude/skills/uc-test/SKILL.md`**:

```markdown
---
name: uc-test
description: Run Umbrella Chronicles port scenarios without the owner — reach title/train gameplay, test controls, measure FPS, capture frames. Use for any live check of uc.exe behaviour, regression or performance.
---
# Umbrella Chronicles test harness
Build first (`build.bat --no-install` stages `dist`). Then from `scripts/`:
- `python -m uctest list` — scenarios.
- `python -m uctest run train_gameplay [--speed 4]` — reach train gameplay, capture; report in `build/autotest/<run>/report.json`.
- `python -m uctest run controls` — fire/reload/grenade/knife/pause must be accepted.
- `python -m uctest run perf --measure 15` — FPS at speed 1 only.
- `--repeat N` for reliability.
Reading results: exit 0 = all passed. On failure open `failure.json` (awaited condition, last frame, capture, log tail, crash report).
Writing a scenario: `from uctest.client import Game; from uctest import steps` — use `g.wait(predicate)`, never `sleep`; menus via `steps.select("<target>")`; actions via `steps.perform`.
Rules: visible window, isolated saves (automatic), no system input, performance only at speed 1, inspect captures yourself.
Protocol and fields: `docs/testing/test-server.md`.
```

- [ ] **Step 5: `AGENTS.md`** (root, short): pointer to the skill file and protocol doc, build command, "never use system input; use `python -m uctest`".
- [ ] **Step 6:** Mark G14 and `train_route.py` superseded (text only; no deletion — owner decides removal). Full `build.bat` (tests + install); final summary with timings.

> Tasks 9–11 implement the spec's Autoplay section (owner-approved 2026-10-15). Task 8's docs get the autoplay sections in Task 11 Step 6.

### Task 9: Reverse engineering — enemies, camera, player health/ammo, result screens

**Files:**
- Create: `docs/spikes/G22-autoplay-memory.md` (findings with addresses, evidence and confidence)
- Modify: `game/input_probe.h/.cpp` (typed reads once verified), `scripts/uctest/menus.py` (Game Over/Continue, chapter result screens)

**Interfaces:**
- Produces (C++, `game/input_probe.h`):

```cpp
struct InputProbeEnemy { uint32_t address = 0; bool alive = false; std::array<float,3> head{}, root{}; float health = -1; };
// appended to InputProbeSnapshot:
std::array<InputProbeEnemy,32> enemies{}; uint32_t enemy_count = 0;
std::array<float,12> view{}; bool view_valid = false;          // camera view matrix 3x4, row-major
int32_t player_health = -1, player_ammo = -1, player_magazine = -1;
uint32_t health_address = 0, ammo_address = 0, magazine_address = 0; // for --god writes
```

- [ ] **Step 1: Player health/ammo.** In train gameplay (server + `controls` scenario), record `state` before/after taking damage and after each shot/reload. Candidate words: the existing `weapon_words` (weapon subobject +480..494) and player object fields near `mode_630`; a word that decrements by one per shot and refills on reload is the magazine; one that drops on a zombie hit is health. Confirm across two runs and one weapon change; write addresses/offsets and evidence to G22.
- [ ] **Step 2: Enemy list.** The player probe already resolves the player object through the game's actor structures; walk the same manager (G22 records the pointer chain): find a list/array whose count matches the visible zombies (train start: 3) and whose entries' float triples move with the zombies. Confirm the alive/dead flag by killing one (count or flag changes). Record pointer chain, entry stride, position offset.
- [ ] **Step 3: Head position.** Per enemy, find the head: either a bone matrix array in the model instance (translation of the head joint) or root position + fixed height. Validate by projecting (Task 10) and checking the point lies on the zombie's head in a capture for 3 zombies at different distances.
- [ ] **Step 4: Camera view matrix.** Find the 3x4 view matrix the scene uses: the camera object near the scene pointer (`scene_address`) or the matrix the game loads with GXLoadPosMtxImm for world geometry with identity model. Verify: world → view with this matrix, then the live native projection and viewport mapping, lands enemy roots on their drawn feet within a few pixels.
- [ ] **Step 5: Result screens.** With the server's UI log, die deliberately (no shooting) to reach Game Over/Continue and finish the train chapter for its result screen; add both screens and their Continue/Next targets (`game_over_continue`, `chapter_result_next`) to `menus.py`, measured like Task 6 Step 1.
- [ ] **Step 6:** Implement the typed reads in `input_probe.cpp` (bounded, checked `memory(c).read` spans, invalid pointers → `valid=false`, never throw on a dead enemy) with a unit test using a synthetic guest memory image (`ppc_fixture.h`) holding one fake enemy list, view matrix and health/ammo words at the recorded offsets.

### Task 10: Enemy projection and data in frame events; `--god`

**Files:**
- Modify: `game/test_state.h/.cpp`, `game/test_server.cpp`, `game/options.h/.cpp`, `game/uc_main.cpp`, `runtime/hle/gx.cpp` (export `gx_current_projection`)
- Test: `tests/test_test_state.cpp`, `tests/test_options.cpp`

**Interfaces:**
- Consumes: Task 9 snapshot fields; native projection via new `std::array<float,16> uc::gx_current_projection()` (projection rows of the last live draw, from `live_transforms.projection`).
- Produces:

```cpp
struct TestEnemy { uint32_t id; bool alive, visible; float head_x, head_y, body_x, body_y, health; };
// TestFrameState gains: std::vector<TestEnemy> enemies; int32_t health=-1, ammo=-1, magazine=-1; bool god=false;
std::optional<std::array<float,2>> project_to_aim(const std::array<float,3>& world, const std::array<float,12>& view, const std::array<float,16>& projection);
// aim coordinates: x = ndc_x, y = -ndc_y (aim y grows downwards)
```

- [ ] **Step 1: Failing test** in `tests/test_test_state.cpp`:

```cpp
    // Projection: straight ahead → centre; behind the camera rejected; aim y points down.
    const std::array<float,12> view{1,0,0,0, 0,1,0,0, 0,0,1,0};
    std::array<float,16> proj{}; proj[0]=1; proj[5]=1; proj[10]=-1; proj[11]=-1; proj[14]=-1; // rows: x'=x, y'=y, z'=-z-1, w'=-z
    auto centre=uc::project_to_aim({0,0,-5},view,proj); CHECK(centre && std::abs((*centre)[0])<1e-6f && std::abs((*centre)[1])<1e-6f);
    auto right=uc::project_to_aim({2.5f,0,-5},view,proj); CHECK(right && std::abs((*right)[0]-0.5f)<1e-5f);
    auto up=uc::project_to_aim({0,2.5f,-5},view,proj); CHECK(up && std::abs((*up)[1]+0.5f)<1e-5f);
    CHECK(!uc::project_to_aim({0,0,5},view,proj));
    uc::TestFrameState e; e.enemies.push_back({7,true,true,0.25f,-0.5f,0.2f,-0.1f,40});
    const auto j=uc::test_frame_json(e,1,0,0,0,0);
    CHECK(j.find(R"("enemies":[{"id":7,"alive":true,"visible":true,"head":[0.2500,-0.5000],"body":[0.2000,-0.1000],"health":40.0}])")!=std::string::npos);
```

The projection array is row-major 4x4 (`proj[r*4+c]`); match it to `gx::Projection`'s storage when wiring `gx_current_projection` and keep the asserted results.

- [ ] **Step 2: Run to verify it fails.**
- [ ] **Step 3: Implement** `project_to_aim` (view × [x,y,z,1] → eye; projection rows × [eye,1] → clip; reject `w<=0`; ndc = clip/w; return `{ndc_x, -ndc_y}`); `visible` = both coordinates in [-1,1]. `test_state_publish` fills `enemies` (alive entries, plus entries that died since the last publish for one frame so the bot sees kills), health/ammo/magazine; JSON adds `"enemies"` and the player fields. `test_frame_json` grows a dynamic buffer (`std::string` building) since the enemy list is variable.
- [ ] **Step 4: `--god`** — option (requires `--test-server`). `test_state_set_god(bool)`; in `game_input_sample` (guest thread, under the baton) when god is on and the snapshot addresses are non-zero, write the maxima recorded in G22 with `ppc::wr32`. Test with a fixture memory: after a sample with god enabled the three words equal the maxima. Frame JSON `"god":true`.
- [ ] **Step 5: Live check** — server on train gameplay; `state` lists 3 enemies with head points; `scripts/uctest/overlay.py <capture> <state.json>` writes `overlay.png` with marks on the heads; inspect it.

### Task 11: Autoplay bot, scenario, logging, docs

**Files:**
- Create: `scripts/uctest/autoplay.py`
- Modify: `scripts/uctest/scenarios.py` (`autoplay`), `scripts/uctest/__main__.py` (`--god`, `--max-minutes`), `.claude/skills/uc-test/SKILL.md`, `docs/testing/test-server.md`, `tests/CMakeLists.txt`
- Test: `tests/test_uctest_autoplay.py`

**Interfaces:**
- Consumes: Task 5 `Game`, Task 6 `steps`/`menus.screen_of`, Task 10 frame fields (`enemies`, `player.magazine`), Task 9 screens (`game_over`, `chapter_result`).
- Produces:

```python
def pick_target(frame: dict, crosshair: tuple[float, float]) -> dict | None   # alive & visible, nearest head to crosshair
class Autoplay:
    def __init__(self, game, log_path, *, stuck_seconds=120, max_failures=5): ...
    def step(self) -> str | None      # one decision from the latest frame; returns a stop reason or None
    def run(self, max_minutes: float) -> dict   # loop; returns the report dict
```

- [ ] **Step 1: Failing test** `tests/test_uctest_autoplay.py`:

```python
import json, sys, tempfile, unittest
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / "scripts"))
from uctest.autoplay import pick_target, Autoplay

def frame(n, context="gameplay", enemies=(), magazine=6, ui=None, tutorial=0x00100000):
    return {"n": n, "context": context, "speed": 1, "ui": ui or [-1]*12, "tutorial": tutorial, "tutorial_loaded": False,
            "player": {"valid": True, "magazine": magazine, "health": 100}, "enemies": list(enemies)}

class FakeGame:
    def __init__(self, frames): self.frames = frames; self.i = 0; self.inputs = []
    @property
    def frame(self): return self.frames[min(self.i, len(self.frames) - 1)]
    def aim(self, x, y, settle_frames=0): self.inputs.append(("aim", round(x, 3), round(y, 3))); self.i += 1; return self.i
    def press(self, button, frames=2, x=0.0, y=0.0): self.inputs.append(("press", button, round(x, 3), round(y, 3))); self.i += 1; return self.i
    def key(self, code, frames=2): self.inputs.append(("key", code)); self.i += 1; return self.i
    def capture(self): return "c.png"
    def wait(self, predicate, frames=None, seconds=60, awaited=""): self.i += 1; return self.frame

E = lambda id, hx, hy, alive=True, visible=True: {"id": id, "alive": alive, "visible": visible, "head": [hx, hy], "body": [hx, hy + 0.2]}

class AutoplayTest(unittest.TestCase):
    def test_pick_nearest_visible_alive(self):
        f = frame(1, enemies=[E(1, 0.8, 0.0), E(2, 0.1, 0.1), E(3, 0.0, 0.0, alive=False), E(4, 0.05, 0.0, visible=False)])
        self.assertEqual(pick_target(f, (0, 0))["id"], 2)
    def test_shoots_head(self):
        g = FakeGame([frame(1, enemies=[E(5, 0.3, -0.4)])] * 3)
        with tempfile.TemporaryDirectory() as d:
            Autoplay(g, Path(d) / "a.jsonl").step()
        self.assertIn(("press", "B", 0.3, -0.4), g.inputs)
    def test_reloads_when_empty(self):
        g = FakeGame([frame(1, magazine=0)] * 3)
        with tempfile.TemporaryDirectory() as d:
            Autoplay(g, Path(d) / "a.jsonl").step()
        self.assertTrue(any(i[0] == "key" for i in g.inputs))
    def test_menu_advances(self):
        g = FakeGame([frame(1, context="menu")] * 3)
        with tempfile.TemporaryDirectory() as d:
            Autoplay(g, Path(d) / "a.jsonl").step()
        self.assertTrue(any(i[0] == "press" and i[1] == "A" for i in g.inputs))
    def test_stuck_stops_with_reason(self):
        frames = [frame(n, enemies=[]) for n in range(0, 20000, 600)]
        g = FakeGame(frames)
        with tempfile.TemporaryDirectory() as d:
            bot = Autoplay(g, Path(d) / "a.jsonl", stuck_seconds=60)
            reason = None
            for _ in range(len(frames)):
                reason = bot.step()
                if reason: break
            self.assertEqual(reason, "stuck")
            lines = [json.loads(l) for l in (Path(d) / "a.jsonl").read_text().splitlines()]
            self.assertEqual(lines[-1]["event"], "stop")

if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
```

CMake: `add_test(NAME test_uctest_autoplay COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test_uctest_autoplay.py ${CMAKE_SOURCE_DIR})`.

- [ ] **Step 2: Run to verify it fails** (`ModuleNotFoundError: uctest.autoplay`).
- [ ] **Step 3: Implement** `autoplay.py`:
  - `pick_target`: filter `alive and visible`; key = squared distance from `crosshair` to `head`; return the minimum or `None`.
  - `Autoplay.step`: read `g.frame`; progress marker = `(screen_of(frame), kills, furthest screen)`; game seconds = `frame["n"] / 60`; marker unchanged for `stuck_seconds` game seconds → log `{"event":"stop","reason":"stuck"}`, return `"stuck"`.
    - `context=="gameplay"`: `player.magazine==0` → `g.key(RELOAD_SCANCODE)` (constant from Task 6 `steps.py`), log `reload`; elif target → `g.aim(*head)`, `g.press("B", 2, *head)`, log `shot` with target id; enemies alive last step and now dead/absent → log `kill`, increment kills.
    - `context=="qte"`: shake QTE = alternate `g.aim(±0.8, 0)` 10 times; prompt = `g.press("A")`; log `qte`.
    - `context=="menu"`: Game Over screen → `steps.select(g, "game_over_continue")`, log `death`, count a failure at the current marker (≥ `max_failures` → stop `"stuck"`); chapter result → `steps.select(g, "chapter_result_next")`, log `chapter`; anything else → `g.press("A")`, log `menu`.
    - `context=="inactive"` (movie/loading) → `g.press("A")` at most every 30 frames.
    - Every 1800 game frames: `g.capture()`, log `capture` with the path.
  - `run(max_minutes)`: loop `step()` until a reason, `max_minutes` of wall time (`"time_limit"`) or `StepFailed` from game exit (`"crashed"`, include `artifacts["crash_report"]`); return `{reason, frames, kills, deaths, furthest_screen, god, speed}` and append it to the log.
  - JSON lines `{"t": frame_n, "event": ..., ...}`, flushed per line.
- [ ] **Step 4: Run** autoplay tests → OK; full ctest passes.
- [ ] **Step 5: Live** — `python -m uctest run autoplay --speed 1 --max-minutes 10`, then `--speed 4`, each with and without `--god`; report how far it got; inspect captures; fix root causes until the train chapter completes with `--god` at speed 1 and 4.
- [ ] **Step 6: Docs** — skill: `python -m uctest run autoplay [--god] [--speed k] [--max-minutes m]`, reading `autoplay.jsonl`/`report.json`, "to reach the next crash run autoplay with `--god`; a `crashed` report gives the crash report path"; protocol doc: `enemies`, player health/ammo, `god`.
