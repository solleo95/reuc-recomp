#include "evdev_logic.h"
#include <algorithm>
#include <string_view>
namespace uc {
using namespace evdev;
namespace {
constexpr std::string_view kRemoteName="Nintendo Wii Remote";
// The key codes of the kernel's hid-wiimote driver (drivers/hid/hid-wiimote-modules.c, wiimod_keys_map and the nunchuk module).
constexpr uint16_t kKeyUp=103,kKeyLeft=105,kKeyRight=106,kKeyDown=108,kKeyNext=407,kKeyPrevious=412,kBtn1=0x101,kBtn2=0x102,kBtnA=0x130,kBtnB=0x131,kBtnC=0x132,kBtnZ=0x135,kBtnMode=0x13c;
bool wii_button_of(uint16_t code,WiiButton& button) {
    switch(code) {
    case kKeyLeft: button=WiiButton::left; return true;
    case kKeyRight: button=WiiButton::right; return true;
    case kKeyUp: button=WiiButton::up; return true;
    case kKeyDown: button=WiiButton::down; return true;
    case kKeyNext: button=WiiButton::plus; return true;
    case kKeyPrevious: button=WiiButton::minus; return true;
    case kBtn1: button=WiiButton::one; return true;
    case kBtn2: button=WiiButton::two; return true;
    case kBtnA: button=WiiButton::a; return true;
    case kBtnB: button=WiiButton::b; return true;
    case kBtnMode: button=WiiButton::home; return true;
    case kBtnZ: button=WiiButton::nunchuk_z; return true;
    case kBtnC: button=WiiButton::nunchuk_c; return true;
    default: return false;
    }
}
uint8_t accel_byte(int32_t value) { return uint8_t(std::clamp((int64_t(value)+512)/4,int64_t(0),int64_t(255))); }   // the nodes centre the 10-bit reading on 0; the remote's own byte is its top eight bits
uint8_t stick_byte(int32_t value) { return uint8_t(std::clamp(int64_t(value)+128,int64_t(8),int64_t(247))); }   // wii_parse_report takes a byte outside 8..247 for "no nunchuk"
uint8_t sdl_button_of(uint16_t code) {   // SDL_BUTTON_LEFT 1, MIDDLE 2, RIGHT 3, X1 4, X2 5
    switch(code) {
    case kBtnLeft: return 1;
    case kBtnMiddle: return 2;
    case kBtnRight: return 3;
    case kBtnSide: return 4;
    case kBtnExtra: return 5;
    default: return 0;
    }
}
float normalised(int32_t value,int32_t low,int32_t high) {
    return float(std::clamp((double(value)-low)/(double(high)-low),0.0,1.0));
}
}
bool evdev_bitmap_has(std::string_view words,unsigned bit,unsigned word_bits) {
    if(!word_bits || word_bits>64) return false;
    std::vector<uint64_t> values;
    size_t at=0;
    while(at<words.size()) {
        while(at<words.size() && words[at]==' ') ++at;
        const size_t start=at;
        uint64_t value=0;
        for(;at<words.size() && words[at]!=' ';++at) {
            const char c=words[at];
            const int digit=c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : c>='A' && c<='F' ? c-'A'+10 : -1;
            if(digit<0 || at-start>=16) return false;   // not a bitmap
            value=value<<4|uint64_t(digit);
        }
        if(at>start) values.push_back(value);
    }
    const size_t word=bit/word_bits;
    if(word>=values.size()) return false;
    return (values[values.size()-1-word]>>(bit%word_bits)&1u)!=0;
}
EvdevCaps evdev_caps_from_sysfs(std::string name,std::string unique,std::string_view key,std::string_view rel,std::string_view abs,std::string_view properties,unsigned word_bits) {
    constexpr unsigned kKeyA=30,kToolPen=0x140,kToolFinger=0x145,kPropDirect=1;
    EvdevCaps caps; caps.name=std::move(name); caps.unique=std::move(unique);
    caps.rel_x=evdev_bitmap_has(rel,kRelX,word_bits); caps.rel_y=evdev_bitmap_has(rel,kRelY,word_bits);
    caps.abs_x=evdev_bitmap_has(abs,kAbsX,word_bits); caps.abs_y=evdev_bitmap_has(abs,kAbsY,word_bits);
    caps.btn_left=evdev_bitmap_has(key,kBtnLeft,word_bits);
    caps.tool_finger=evdev_bitmap_has(key,kToolFinger,word_bits); caps.tool_pen=evdev_bitmap_has(key,kToolPen,word_bits);
    caps.keyboard_keys=evdev_bitmap_has(key,kKeyA,word_bits);
    caps.direct=evdev_bitmap_has(properties,kPropDirect,word_bits);
    return caps;
}
std::string evdev_group_key(std::string_view hid_parent,std::string_view phys) {
    constexpr std::string_view kVirtual="/sys/devices/virtual";
    if(hid_parent.empty()) return {};
    if(hid_parent.substr(0,kVirtual.size())==kVirtual) return std::string(phys);
    return std::string(hid_parent);
}
int evdev_led_player(std::string_view led_name) {
    constexpr std::string_view kTail=":blue:p";
    if(led_name.size()<kTail.size()+1) return -1;
    const auto tail=led_name.substr(led_name.size()-kTail.size()-1);
    if(tail.substr(0,kTail.size())!=kTail) return -1;
    const char digit=tail.back();
    return digit>='0' && digit<='3' ? digit-'0' : -1;
}
EvdevRole classify_evdev(const EvdevCaps& caps) {
    if(std::string_view(caps.name).substr(0,kRemoteName.size())==kRemoteName) {
        const auto suffix=std::string_view(caps.name).substr(kRemoteName.size());
        if(suffix.empty()) return EvdevRole::wiimote_buttons;
        if(suffix==" Accelerometer") return EvdevRole::wiimote_accel;
        if(suffix==" IR") return EvdevRole::wiimote_ir;
        if(suffix==" Nunchuk") return EvdevRole::wiimote_nunchuk;
        return EvdevRole::ignore;   // Motion Plus, Pro Controller, Balance Board, ...
    }
    if(caps.tool_finger || caps.tool_pen || caps.direct || caps.keyboard_keys) return EvdevRole::ignore;
    if(caps.rel_x && caps.rel_y && caps.btn_left) return EvdevRole::mouse;
    if(caps.abs_x && caps.abs_y && caps.btn_left && caps.abs_max[0]>caps.abs_min[0] && caps.abs_max[1]>caps.abs_min[1]) return EvdevRole::gun;
    return EvdevRole::ignore;
}
void feed_pointer(PointerState& state,const EvdevCaps& caps,std::span<const EvdevEvent> events,uint64_t device,std::vector<PointerEvent>& out) {
    if(!state.started) { state.abs=caps.abs_value; state.started=true; }
    for(const auto& event:events) {
        if(event.type==kKey) {
            const uint8_t button=sdl_button_of(event.code);
            if(button && (event.value==0 || event.value==1)) state.buttons.emplace_back(button,event.value==1);   // value 2 is a key repeat
        } else if(event.type==kRel) {
            if(event.code==kRelX) state.dx+=float(event.value);
            else if(event.code==kRelY) state.dy+=float(event.value);
            else if(event.code==kRelWheel) state.wheel+=float(event.value);
        } else if(event.type==kAbs) {
            if((event.code==kAbsX || event.code==kAbsY) && state.abs[event.code]!=event.value) { state.abs[event.code]=event.value; state.abs_dirty=true; }
        } else if(event.type==kSyn && event.code==kSynReport) {
            if(state.dx!=0 || state.dy!=0) {
                PointerEvent move; move.kind=PointerEvent::Kind::relative; move.device=device; move.x=state.dx; move.y=state.dy;
                out.push_back(move);
            }
            if(state.abs_dirty) {
                PointerEvent at; at.kind=PointerEvent::Kind::absolute; at.device=device;
                at.x=normalised(state.abs[0],caps.abs_min[0],caps.abs_max[0]); at.y=normalised(state.abs[1],caps.abs_min[1],caps.abs_max[1]);
                out.push_back(at);
            }
            if(state.wheel!=0) {
                PointerEvent turn; turn.kind=PointerEvent::Kind::wheel; turn.device=device; turn.x=state.wheel;
                out.push_back(turn);
            }
            for(const auto& [button,down]:state.buttons) {   // after the motion: a shot lands where the pointer is now
                PointerEvent press; press.kind=PointerEvent::Kind::button; press.device=device; press.button=button; press.down=down;
                out.push_back(press);
            }
            state.dx=state.dy=state.wheel=0; state.abs_dirty=false; state.buttons.clear();
        }
    }
}
void feed_wiimote(WiiEvdevState& wii,EvdevRole role,std::span<const EvdevEvent> events) {
    auto& state=wii.state;
    for(const auto& event:events) {
        if(event.type==kKey) {
            WiiButton button;
            if(!wii_button_of(event.code,button) || (event.value!=0 && event.value!=1)) continue;
            const bool nunchuk_key=button==WiiButton::nunchuk_z || button==WiiButton::nunchuk_c;
            if(nunchuk_key!=(role==EvdevRole::wiimote_nunchuk)) continue;   // each key belongs to its own node
            state.buttons=event.value ? state.buttons|1u<<unsigned(button) : state.buttons&~(1u<<unsigned(button));
        } else if(event.type==kAbs) {
            if(role==EvdevRole::wiimote_accel || role==EvdevRole::wiimote_nunchuk) {
                auto& bytes=role==EvdevRole::wiimote_accel ? state.accel : state.nunchuk_accel;
                if(event.code>=kAbsRx && event.code<=kAbsRz) bytes[event.code-kAbsRx]=accel_byte(event.value);
                else if(role==EvdevRole::wiimote_nunchuk && (event.code==kAbsHat0X || event.code==kAbsHat0X+1)) state.stick[event.code-kAbsHat0X]=stick_byte(event.value);
            } else if(role==EvdevRole::wiimote_ir && event.code>=kAbsHat0X && event.code<kAbsHat0X+8) {
                const unsigned dot=(event.code-kAbsHat0X)/2; const bool is_y=((event.code-kAbsHat0X)&1)!=0;
                auto& ir=state.ir[dot];
                (is_y ? ir.y : ir.x)=uint16_t(std::clamp(event.value,0,1023));
                (is_y ? wii.seen_y : wii.seen_x)[dot]=true;
                ir.valid=wii.seen_x[dot] && wii.seen_y[dot] && !(ir.x==1023 && ir.y==1023);   // 1023,1023 is how the kernel reports a dot the camera does not see
            }
        }
    }
}
}
