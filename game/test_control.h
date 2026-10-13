#pragma once
#include <cmath>
#include <charconv>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
namespace uc {
struct TestCommand { uint64_t sequence; float x,y; uint32_t held; bool capture; };
inline std::optional<TestCommand> parse_test_command(std::string_view text) {
    if(text.size()>128 || text.empty()) return {};
    std::istringstream stream{std::string(text)};
    TestCommand result{}; unsigned capture=0;
    std::string sequence;
    if(!(stream>>sequence>>result.x>>result.y>>result.held>>capture)) return {};
    const auto parsed=std::from_chars(sequence.data(),sequence.data()+sequence.size(),result.sequence);
    if(parsed.ec!=std::errc{} || parsed.ptr!=sequence.data()+sequence.size()) return {};
    stream>>std::ws;
    if(!stream.eof() || !result.sequence || !std::isfinite(result.x) || !std::isfinite(result.y) ||
       std::abs(result.x)>1 || std::abs(result.y)>1 || (result.held&~0x1C10u) || capture>1) return {};
    result.capture=capture!=0; return result;
}
}
