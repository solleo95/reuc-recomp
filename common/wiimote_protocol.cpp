#include "wiimote_protocol.h"
#include <algorithm>
#include <stdexcept>
namespace uc {
namespace {
struct Layout { uint8_t id; size_t size; size_t ir; size_t ir_bytes; size_t extension; };   // offsets in the report; 0 = absent
constexpr Layout kLayouts[]={{0x30,3,0,0,0},{0x31,6,0,0,0},{0x33,18,6,12,0},{0x35,22,0,0,6},{0x37,22,6,10,16}};
uint32_t buttons_of(uint8_t first,uint8_t second) {
    uint32_t buttons=0;
    const auto set=[&](bool on,WiiButton button) { if(on) buttons|=1u<<unsigned(button); };
    set(first&0x01,WiiButton::left); set(first&0x02,WiiButton::right); set(first&0x04,WiiButton::down); set(first&0x08,WiiButton::up); set(first&0x10,WiiButton::plus);
    set(second&0x01,WiiButton::two); set(second&0x02,WiiButton::one); set(second&0x04,WiiButton::b); set(second&0x08,WiiButton::a); set(second&0x10,WiiButton::minus); set(second&0x80,WiiButton::home);
    return buttons;
}
WiiIrDot dot(uint8_t low_x,uint8_t low_y,unsigned high_x,unsigned high_y) {
    WiiIrDot result{uint16_t(low_x|(high_x&3)<<8),uint16_t(low_y|(high_y&3)<<8),true};
    result.valid=!(low_x==0xFF && low_y==0xFF && (high_x&3)==3 && (high_y&3)==3);   // 0xFF.. in every bit: nothing seen
    return result;
}
}
bool wii_parse_report(std::span<const uint8_t> report,WiiState& state) {
    if(report.empty()) return false;
    const auto* layout=std::find_if(std::begin(kLayouts),std::end(kLayouts),[&](const Layout& l){return l.id==report[0];});
    if(layout==std::end(kLayouts) || report.size()<layout->size) return false;
    WiiState next;
    next.buttons=buttons_of(report[1],report[2]);
    if(layout->size>=6) next.accel={report[3],report[4],report[5]};
    if(layout->ir) {
        const uint8_t* ir=report.data()+layout->ir;
        if(layout->ir_bytes==10) {   // basic: two dots in five bytes, the high bits shared in the middle one
            for(unsigned pair=0;pair<2;++pair) {
                const uint8_t* b=ir+5*pair;
                next.ir[2*pair]=dot(b[0],b[1],b[2]>>4,b[2]>>6);
                next.ir[2*pair+1]=dot(b[3],b[4],b[2],b[2]>>2);
            }
        } else for(unsigned i=0;i<4;++i) {   // extended: three bytes a dot (and its size, unused)
            const uint8_t* b=ir+3*i;
            next.ir[i]=dot(b[0],b[1],b[2]>>4,b[2]>>6);
        }
    }
    if(layout->extension) {
        const uint8_t* ext=report.data()+layout->extension;
        // shortcut: no extension reads as all 0x00 or all 0xFF, and a stick never reaches the extremes of a byte (about 30..225), so such bytes are encrypted, uninitialised or
        // not a nunchuk. WiimoteInput also asks the status report whether an extension is plugged in. Upgrade when other extensions matter.
        const bool absent=(std::all_of(ext,ext+6,[&](uint8_t b){return b==ext[0];}) && (ext[0]==0x00 || ext[0]==0xFF))
            || ext[0]<8 || ext[0]>247 || ext[1]<8 || ext[1]>247;
        if(!absent) {
            next.has_nunchuk=true; next.stick={ext[0],ext[1]}; next.nunchuk_accel={ext[2],ext[3],ext[4]};
            if(!(ext[5]&0x01)) next.buttons|=1u<<unsigned(WiiButton::nunchuk_z);   // the nunchuk's buttons are active low
            if(!(ext[5]&0x02)) next.buttons|=1u<<unsigned(WiiButton::nunchuk_c);
        }
    }
    state=next;
    return true;
}
std::optional<WiiStatus> wii_parse_status(std::span<const uint8_t> report) {
    if(report.size()<7 || report[0]!=0x20) return std::nullopt;
    return WiiStatus{(report[3]&0x02)!=0,report[6]};
}
std::array<uint8_t,2> wii_led_report(unsigned player) {
    if(player>4) throw std::invalid_argument("a Wii Remote has four player LEDs");
    return {0x11,uint8_t(player ? 0x10<<(player-1) : 0)};
}
std::array<uint8_t,3> wii_reporting_report(uint8_t mode) { return {0x12,0x04,mode}; }
std::vector<uint8_t> wii_write_register(uint32_t address,std::span<const uint8_t> data) {
    if(address>0xFFFFFF || data.size()>16) throw std::invalid_argument("register write out of range");
    std::vector<uint8_t> report(22,0);
    report[0]=0x16; report[1]=0x04;   // 0x04: the control registers (without it the write goes to the EEPROM)
    report[2]=uint8_t(address>>16); report[3]=uint8_t(address>>8); report[4]=uint8_t(address);
    report[5]=uint8_t(data.size());
    std::copy(data.begin(),data.end(),report.begin()+6);
    return report;
}
namespace {
constexpr unsigned kWrite=50;   // the IR camera ignores writes that come closer
WiiStep write_step(uint32_t address,std::initializer_list<uint8_t> bytes) { return {wii_write_register(address,std::vector<uint8_t>(bytes)),kWrite}; }
}
std::vector<WiiStep> wii_extension_steps() {
    return {write_step(0xA400F0,{0x55}),write_step(0xA400FB,{0x00})};   // the extension without encryption: its data reads as plain bytes
}
std::array<uint8_t,2> wii_status_request() { return {0x15,0x00}; }
std::vector<WiiStep> wii_init_sequence() {
    std::vector<WiiStep> steps=wii_extension_steps();
    const auto write=[&](uint32_t address,std::initializer_list<uint8_t> bytes) { steps.push_back(write_step(address,bytes)); };
    steps.push_back({{0x13,0x04},kWrite}); steps.push_back({{0x1A,0x04},kWrite});   // the IR camera's two enables
    write(0xB00030,{0x08});
    write(0xB00000,{0x02,0x00,0x00,0x71,0x01,0x00,0xAA,0x00,0x64});   // sensitivity level 3, block 1
    write(0xB0001A,{0x63,0x03});                                        // block 2
    write(0xB00033,{0x01});                                             // basic mode: ten bytes for four dots
    write(0xB00030,{0x08});
    const auto mode=wii_reporting_report(0x37);
    steps.push_back({std::vector<uint8_t>(mode.begin(),mode.end()),0});
    const auto led=wii_led_report(1);
    steps.push_back({std::vector<uint8_t>(led.begin(),led.end()),0});
    return steps;
}
}
