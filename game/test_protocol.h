#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
namespace uc {
enum class TestDevice : uint8_t { kpad, key, mouse, wheel, motion };
struct TestInput { uint64_t at_frame = 0; uint32_t hold_frames = 1; TestDevice device = TestDevice::kpad;
                   float x = 0, y = 0; uint32_t buttons = 0, code = 0; bool down = false; int delta = 0; };
enum class TestRequestKind : uint8_t { input, capture, state, speed, quit, peek, draws, host_key, options };
struct TestRequest { uint64_t id = 0; TestRequestKind kind = TestRequestKind::state; TestInput input; uint32_t factor = 1, addr = 0, size = 0; std::string text; };   // text: options JSON
struct TestParseError { uint64_t id; std::string message; };   // id 0 when unknown
std::variant<TestRequest, TestParseError> parse_test_request(std::string_view line);
std::string test_reply_ok(uint64_t id, std::string_view extra_fields = {});   // extra like "\"path\":\"...\"" (already JSON)
std::string test_reply_error(uint64_t id, std::string_view message);
std::string json_string(std::string_view text);   // quoted, escaped
}
