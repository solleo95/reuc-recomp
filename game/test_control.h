#pragma once
#include <cmath>
#include <charconv>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
namespace uc {
enum class TestKind:uint8_t { legacy,key,mouse,wheel,motion,focus };
struct TestCommand { uint64_t sequence; float x,y; uint32_t held; bool capture; TestKind kind=TestKind::legacy; uint32_t code=0,value=0; };
inline std::optional<TestCommand> parse_test_command(std::string_view text) {
    if(text.size()>256 || text.empty()) return {};
    std::istringstream stream{std::string(text)};
    TestCommand result{}; unsigned capture=0;
    std::string sequence;
    if(!(stream>>sequence))return {};
    if(sequence=="v2") {
        std::string kind,code,value;
        if(!(stream>>sequence>>kind>>code>>value>>result.x>>result.y>>capture))return {};
        const auto integer=[](const std::string& text,uint32_t& output) {
            const auto parsed=std::from_chars(text.data(),text.data()+text.size(),output);
            return parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size();
        };
        if(!integer(code,result.code) || !integer(value,result.value))return {};
        if(kind=="key")result.kind=TestKind::key;
        else if(kind=="mouse")result.kind=TestKind::mouse;
        else if(kind=="wheel")result.kind=TestKind::wheel;
        else if(kind=="motion")result.kind=TestKind::motion;
        else if(kind=="focus")result.kind=TestKind::focus;
        else return {};
        switch(result.kind) {
        case TestKind::key: if(!result.code || result.code>=512 || result.value>1)return {}; break;
        case TestKind::mouse: if(!result.code || result.code>=256 || result.value>1)return {}; break;
        case TestKind::wheel: if((result.code!=1 && result.code!=2) || !result.value || result.value>256)return {}; break;
        case TestKind::motion: if(result.code || result.value)return {}; break;
        case TestKind::focus: if(result.code || result.value>1)return {}; break;
        case TestKind::legacy: return {};
        }
        if(result.kind!=TestKind::motion && (result.x!=0 || result.y!=0))return {};
    } else if(!(stream>>result.x>>result.y>>result.held>>capture))return {};
    const auto parsed=std::from_chars(sequence.data(),sequence.data()+sequence.size(),result.sequence);
    if(parsed.ec!=std::errc{} || parsed.ptr!=sequence.data()+sequence.size()) return {};
    stream>>std::ws;
    if(!stream.eof() || !result.sequence || !std::isfinite(result.x) || !std::isfinite(result.y) ||
       (result.kind==TestKind::legacy && (std::abs(result.x)>1 || std::abs(result.y)>1 || (result.held&~0x1C10u))) || capture>1) return {};
    result.capture=capture!=0; return result;
}
}
