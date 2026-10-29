#include "check.h"
#include "wiimote_protocol.h"
#include <algorithm>
#include <stdexcept>
using namespace uc;
static uint32_t bit(WiiButton button) { return 1u<<unsigned(button); }
static bool has_report(const std::vector<WiiStep>& steps,uint8_t id) {
    return std::any_of(steps.begin(),steps.end(),[id](const WiiStep& s){return s.report.front()==id;});
}
static size_t first_write_to(const std::vector<WiiStep>& steps,uint32_t address) {
    for(size_t i=0;i<steps.size();++i) {
        const auto& r=steps[i].report;
        if(r[0]==0x16 && r[1]==0x04 && r[2]==(address>>16 & 255) && r[3]==(address>>8 & 255) && r[4]==(address & 255)) return i;
    }
    return SIZE_MAX;
}
static void parses_buttons() {
    WiiState state;
    const uint8_t a_b_home[]={0x30,0x00,0x8C};   // second byte: 0x08 A, 0x04 B, 0x80 Home
    CHECK(wii_parse_report(a_b_home,state));
    CHECK(state.buttons==(bit(WiiButton::a)|bit(WiiButton::b)|bit(WiiButton::home)));
    const uint8_t dpad[]={0x30,0x01|0x08|0x10,0x01|0x02|0x10};   // left, up, plus / two, one, minus
    CHECK(wii_parse_report(dpad,state));
    CHECK(state.buttons==(bit(WiiButton::left)|bit(WiiButton::up)|bit(WiiButton::plus)|bit(WiiButton::two)|bit(WiiButton::one)|bit(WiiButton::minus)));
    const uint8_t right_down[]={0x30,0x02|0x04,0x00};
    CHECK(wii_parse_report(right_down,state) && state.buttons==(bit(WiiButton::right)|bit(WiiButton::down)));
}
static void parses_the_accelerometer() {
    WiiState state;
    const uint8_t report[]={0x31,0x00,0x00,0x7F,0x90,0xB3};
    CHECK(wii_parse_report(report,state));
    CHECK((state.accel==std::array<uint8_t,3>{0x7F,0x90,0xB3}));
}
static void parses_0x37_with_ir_and_nunchuk() {
    WiiState state;
    std::vector<uint8_t> report{0x37,0x00,0x08,0x80,0x82,0x9A};
    // basic IR, two dots per five bytes: (300,400) and (700,410), then a pair of unseen dots (all 0xFF)
    report.insert(report.end(),{44,144,0x56,188,154, 0xFF,0xFF,0xFF,0xFF,0xFF});
    report.insert(report.end(),{100,200,0x80,0x80,0xB4,0xFE});   // nunchuk: stick, accelerometer, Z pressed (bit 0 clear), C not
    CHECK(report.size()==22 && wii_parse_report(report,state));
    CHECK(state.ir[0].valid && state.ir[0].x==300 && state.ir[0].y==400);
    CHECK(state.ir[1].valid && state.ir[1].x==700 && state.ir[1].y==410);
    CHECK(!state.ir[2].valid && !state.ir[3].valid);
    CHECK(state.has_nunchuk && state.stick[0]==100 && state.stick[1]==200);
    CHECK((state.buttons&bit(WiiButton::nunchuk_z))!=0 && (state.buttons&bit(WiiButton::nunchuk_c))==0 && (state.buttons&bit(WiiButton::a))!=0);
}
static void parses_0x33_extended_ir() {
    WiiState state;
    std::vector<uint8_t> report{0x33,0x00,0x00,0x80,0x80,0x80};
    report.insert(report.end(),{44,144,0x55, 0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF});   // dot 0 at (300,400): y high bits 1 (0x40), x high bits 1 (0x10), size 5
    CHECK(report.size()==18 && wii_parse_report(report,state));
    CHECK(state.ir[0].valid && state.ir[0].x==300 && state.ir[0].y==400);
    CHECK(!state.ir[1].valid && !state.ir[2].valid && !state.ir[3].valid);
}
static void no_extension_means_no_nunchuk() {
    WiiState state;
    for(uint8_t filler:{uint8_t(0x00),uint8_t(0xFF)}) {
        std::vector<uint8_t> report{0x37,0,0,0x80,0x80,0x80};
        report.insert(report.end(),10,0xFF); report.insert(report.end(),6,filler);
        CHECK(wii_parse_report(report,state) && !state.has_nunchuk && state.stick[0]==128 && (state.buttons&(bit(WiiButton::nunchuk_z)|bit(WiiButton::nunchuk_c)))==0);
    }
}
static void implausible_extension_bytes_are_not_a_nunchuk() {
    WiiState state;
    for(const std::array<uint8_t,2> stick:{std::array<uint8_t,2>{3,128},{128,250},{0xFF,0x37}}) {   // encrypted or uninitialised data lands outside what a stick can reach
        std::vector<uint8_t> report{0x37,0,0,0x80,0x80,0x80}; report.insert(report.end(),10,0xFF); report.insert(report.end(),{stick[0],stick[1],0x80,0x80,0xB4,0x00});
        CHECK(wii_parse_report(report,state) && !state.has_nunchuk && (state.buttons&(bit(WiiButton::nunchuk_z)|bit(WiiButton::nunchuk_c)))==0);
    }
}
static void rejects_short_and_unknown_reports() {
    WiiState state; state.buttons=0x55;
    const uint8_t short30[]={0x30,0x00};
    const uint8_t unknown[]={0x3E,0,0,0,0,0,0,0};
    std::vector<uint8_t> cut37{0x37,0,0,0x80,0x80,0x80}; cut37.insert(cut37.end(),15,0);   // one byte short of 22
    CHECK(!wii_parse_report({},state));
    CHECK(!wii_parse_report(short30,state));
    CHECK(!wii_parse_report(unknown,state));
    CHECK(!wii_parse_report(cut37,state));
    std::vector<uint8_t> status{0x20,0,0,0x02,0,0,0xC0};
    CHECK(!wii_parse_report(status,state));   // a status report is not a data report
    CHECK(state.buttons==0x55);               // a refused report leaves the state alone
}
static void parses_status() {
    const std::vector<uint8_t> with_extension{0x20,0,0,0x02|0x10,0,0,0xC0};
    const auto status=wii_parse_status(with_extension);
    CHECK(status && status->extension && status->battery==0xC0);
    const std::vector<uint8_t> without{0x20,0,0,0x10,0,0,0x40};
    CHECK(wii_parse_status(without) && !wii_parse_status(without)->extension);
    const uint8_t data[]={0x30,0,0};
    CHECK(!wii_parse_status(data) && !wii_parse_status({}));
    const std::vector<uint8_t> shortened{0x20,0,0,0x02};
    CHECK(!wii_parse_status(shortened));
}
static void builds_reports() {
    CHECK((wii_led_report(1)==std::array<uint8_t,2>{0x11,0x10}));
    CHECK((wii_led_report(4)==std::array<uint8_t,2>{0x11,0x80}));
    CHECK((wii_led_report(0)==std::array<uint8_t,2>{0x11,0x00}));
    bool refused=false; try { wii_led_report(5); } catch(const std::invalid_argument&) { refused=true; }
    CHECK(refused);
    CHECK((wii_reporting_report(0x37)==std::array<uint8_t,3>{0x12,0x04,0x37}));
    const uint8_t one[]={0x55};
    const auto write=wii_write_register(0xA400F0,one);
    CHECK(write.size()==22 && write[0]==0x16 && write[1]==0x04 && write[2]==0xA4 && write[3]==0x00 && write[4]==0xF0 && write[5]==1 && write[6]==0x55);
    CHECK(std::all_of(write.begin()+7,write.end(),[](uint8_t b){return b==0;}));
    const std::vector<uint8_t> seventeen(17,1);
    refused=false; try { wii_write_register(0xB00000,seventeen); } catch(const std::invalid_argument&) { refused=true; }
    CHECK(refused);
}
static void init_sequence_order() {
    const auto steps=wii_init_sequence();
    CHECK(!steps.empty());
    const size_t extension=first_write_to(steps,0xA400F0),extension2=first_write_to(steps,0xA400FB),ir_power=first_write_to(steps,0xB00030),blocks=first_write_to(steps,0xB00000);
    CHECK(extension!=SIZE_MAX && extension2==extension+1);                      // the extension is made plain (unencrypted) first
    CHECK(has_report(steps,0x13) && has_report(steps,0x1A));
    CHECK(ir_power!=SIZE_MAX && blocks!=SIZE_MAX && first_write_to(steps,0xB0001A)!=SIZE_MAX && first_write_to(steps,0xB00033)!=SIZE_MAX);
    CHECK(extension2<ir_power && ir_power<blocks);
    for(const auto& step:steps) if(step.report[0]==0x16 && step.report[2]==0xB0) CHECK(step.delay_ms>=50);   // the camera needs time between its writes
    CHECK(steps[steps.size()-2].report[0]==0x12 && steps[steps.size()-2].report[2]==0x37);   // the data mode, then the LED
    CHECK((steps.back().report==std::vector<uint8_t>{0x11,0x10}));
    const auto extension_only=wii_extension_steps();   // plugging a nunchuk in later repeats just its part
    CHECK(extension_only.size()==2 && extension_only[0].report==steps[0].report && extension_only[1].report==steps[1].report);
    CHECK((wii_status_request()==std::array<uint8_t,2>{0x15,0x00}));
}
int main() {
    parses_buttons();
    parses_the_accelerometer();
    parses_0x37_with_ir_and_nunchuk();
    parses_0x33_extended_ir();
    no_extension_means_no_nunchuk();
    implausible_extension_bytes_are_not_a_nunchuk();
    rejects_short_and_unknown_reports();
    parses_status();
    builds_reports();
    init_sequence_order();
}
