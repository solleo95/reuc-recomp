#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
namespace uc {
// pad_button: code 0..31 (SDL gamepad button), pad_axis: code 0..15 (axis*2 + direction), both held like keys (down + hold_frames); pad_stick: code 0 left / 1 right, x/y -1..1
// (y down positive), stays until changed; pad_gyro: x yaw, y pitch in rad/s (turning left / tilting up positive), a rate that stays until changed.
// aim: the pointer put at x/y -1..1 of the window (y down positive) like a lightgun's absolute position; the jump feeds the swing detectors.
// wii_button: code 0..12 (WiiButton), held like keys; wii_pointer: the IR pointer x/y -1..1, stays until changed; wii_stick: the nunchuk stick x/y -1..1 (y down), stays until
// changed; wii_motion: one swing as mouse counts x/y.
enum class TestDevice : uint8_t { kpad, key, mouse, wheel, motion, pad_button, pad_axis, pad_stick, pad_gyro, aim, wii_button, wii_pointer, wii_stick, wii_motion };
struct TestInput { uint64_t at_frame = 0; uint32_t hold_frames = 1; TestDevice device = TestDevice::kpad;
                   float x = 0, y = 0; uint32_t buttons = 0, code = 0; bool down = false; int delta = 0; };
enum class TestRequestKind : uint8_t { input, capture, state, speed, quit, peek, draws, host_key, options, menu_click };
struct TestRequest { uint64_t id = 0; TestRequestKind kind = TestRequestKind::state; TestInput input; uint32_t factor = 1, addr = 0, size = 0; float mx = 0, my = 0; std::string text, gamepad, settings; };   // text: graphics options JSON, gamepad: gamepad options JSON, settings: root config keys JSON (pointer, reload_zone, ...); any may be empty
struct TestParseError { uint64_t id; std::string message; };   // id 0 when unknown
std::variant<TestRequest, TestParseError> parse_test_request(std::string_view line);
std::string test_reply_ok(uint64_t id, std::string_view extra_fields = {});   // extra like "\"path\":\"...\"" (already JSON)
std::string test_reply_error(uint64_t id, std::string_view message);
std::string json_string(std::string_view text);   // quoted, escaped
}
