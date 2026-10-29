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
    CHECK(ok(parse_test_request(R"({"id":13,"cmd":"draws"})")).kind==TestRequestKind::draws);
    auto peek=ok(parse_test_request(R"({"id":11,"cmd":"peek","addr":2152422600,"size":64})"));
    CHECK(peek.kind==TestRequestKind::peek && peek.addr==0x804B5CC8u && peek.size==64);
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"peek","addr":2152422600,"size":262145})")).empty());
    CHECK(ok(parse_test_request(R"({"id":12,"cmd":"peek","addr":2147483648,"size":262144})")).size==262144);
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"peek","addr":-1,"size":4})")).empty());
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"peek","addr":4294967296,"size":4})")).empty());
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"peek","addr":2152422600})")).empty());
    // Host keys of the port (Insert menu, P pause) and live settings changes, the paths the in-game menu takes.
    const auto host=ok(parse_test_request(R"({"id":14,"cmd":"host_key","code":73})"));
    CHECK(host.kind==TestRequestKind::host_key && host.input.code==73);
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"host_key"})")).empty());
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"host_key","code":0})")).empty());
    const auto click=ok(parse_test_request(R"({"id":15,"cmd":"menu_click","x":640.5,"y":120})"));   // a click on the in-game menu, window pixels
    CHECK(click.kind==TestRequestKind::menu_click && click.mx==640.5f && click.my==120.f);
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"menu_click","x":10})")).empty());
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"menu_click","x":-1,"y":5})")).empty());
    const auto options=ok(parse_test_request(R"({"id":15,"cmd":"options","graphics":{"bloom":false,"upscaler":"fsr","render_scale":2}})"));
    CHECK(options.kind==TestRequestKind::options && options.text==R"({"bloom":false,"upscaler":"fsr","render_scale":2})");
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"options"})")).empty());
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"options","graphics":5})")).empty());
    // The gamepad: buttons and axis presses (held like keys), stick positions and gyro rates (set until changed).
    const auto pad_button=ok(parse_test_request(R"({"id":16,"cmd":"input","hold_frames":4,"pad_button":{"button":3,"down":true}})"));
    CHECK(pad_button.input.device==TestDevice::pad_button && pad_button.input.code==3 && pad_button.input.down && pad_button.input.hold_frames==4);
    const auto pad_axis=ok(parse_test_request(R"({"id":17,"cmd":"input","pad_axis":{"code":11,"down":true}})"));
    CHECK(pad_axis.input.device==TestDevice::pad_axis && pad_axis.input.code==11 && pad_axis.input.down);
    const auto stick=ok(parse_test_request(R"({"id":18,"cmd":"input","pad_stick":{"side":"right","x":0.5,"y":-1}})"));
    CHECK(stick.input.device==TestDevice::pad_stick && stick.input.code==1 && stick.input.x==.5f && stick.input.y==-1.f);
    CHECK(ok(parse_test_request(R"({"id":19,"cmd":"input","pad_stick":{"side":"left","x":0,"y":0}})")).input.code==0);
    const auto gyro=ok(parse_test_request(R"({"id":20,"cmd":"input","pad_gyro":{"x":1.5,"y":-2}})"));
    CHECK(gyro.input.device==TestDevice::pad_gyro && gyro.input.x==1.5f && gyro.input.y==-2.f);
    const auto aim_request=ok(parse_test_request(R"({"id":30,"cmd":"input","aim":{"x":0.5,"y":-0.25}})"));
    CHECK(aim_request.input.device==TestDevice::aim && aim_request.input.x==.5f && aim_request.input.y==-.25f);
    for(auto text:{R"({"id":1,"cmd":"input","aim":{"x":2,"y":0}})",R"({"id":1,"cmd":"input","aim":{"x":0}})",R"({"id":1,"cmd":"input","hold_frames":2,"aim":{"x":0,"y":0}})"})
        CHECK(!err(parse_test_request(text)).empty());
    const auto wii_button=ok(parse_test_request(R"({"id":33,"cmd":"input","hold_frames":3,"wii_button":{"button":7,"down":true}})"));
    CHECK(wii_button.input.device==TestDevice::wii_button && wii_button.input.code==7 && wii_button.input.down && wii_button.input.hold_frames==3);
    const auto wii_pointer=ok(parse_test_request(R"({"id":34,"cmd":"input","wii_pointer":{"x":0.5,"y":-0.5}})"));
    CHECK(wii_pointer.input.device==TestDevice::wii_pointer && wii_pointer.input.x==.5f && wii_pointer.input.y==-.5f);
    const auto wii_stick=ok(parse_test_request(R"({"id":35,"cmd":"input","wii_stick":{"x":1,"y":0}})"));
    CHECK(wii_stick.input.device==TestDevice::wii_stick && wii_stick.input.x==1.f);
    const auto wii_motion=ok(parse_test_request(R"({"id":36,"cmd":"input","wii_motion":{"x":300,"y":-20}})"));
    CHECK(wii_motion.input.device==TestDevice::wii_motion && wii_motion.input.x==300.f && wii_motion.input.y==-20.f);
    for(auto text:{R"({"id":1,"cmd":"input","wii_button":{"button":13,"down":true}})",R"({"id":1,"cmd":"input","wii_button":{"button":3}})",
        R"({"id":1,"cmd":"input","wii_pointer":{"x":2,"y":0}})",R"({"id":1,"cmd":"input","wii_stick":{"x":0,"y":-1.5}})",R"({"id":1,"cmd":"input","wii_motion":{"x":5000,"y":0}})",
        R"({"id":1,"cmd":"input","hold_frames":2,"wii_pointer":{"x":0,"y":0}})",R"({"id":1,"cmd":"input","hold_frames":2,"wii_stick":{"x":0,"y":0}})",
        R"({"id":1,"cmd":"input","hold_frames":2,"wii_motion":{"x":0,"y":0}})"}) CHECK(!err(parse_test_request(text)).empty());
    const auto root_options=ok(parse_test_request(R"({"id":31,"cmd":"options","settings":{"reload_zone":"corner","pointer":"absolute"}})"));
    CHECK(root_options.kind==TestRequestKind::options && root_options.settings.find("reload_zone")!=std::string::npos);
    CHECK(!err(parse_test_request(R"({"id":32,"cmd":"options","settings":1})")).empty());
    for(auto text:{R"({"id":1,"cmd":"input","pad_button":{"button":32,"down":true}})",R"({"id":1,"cmd":"input","pad_button":{"button":-1,"down":true}})",
        R"({"id":1,"cmd":"input","pad_button":{"button":3}})",R"({"id":1,"cmd":"input","pad_axis":{"code":16,"down":true}})",
        R"({"id":1,"cmd":"input","pad_stick":{"side":"up","x":0,"y":0}})",R"({"id":1,"cmd":"input","pad_stick":{"side":"left","x":1.5,"y":0}})",
        R"({"id":1,"cmd":"input","pad_stick":{"x":0,"y":0}})",R"({"id":1,"cmd":"input","pad_gyro":{"x":100,"y":0}})",
        R"({"id":1,"cmd":"input","hold_frames":2,"pad_stick":{"side":"left","x":0,"y":0}})",R"({"id":1,"cmd":"input","hold_frames":2,"pad_gyro":{"x":0,"y":0}})",
        R"({"id":1,"cmd":"input","pad_gyro":{"x":0,"y":0},"key":{"code":4,"down":true}})"}) CHECK(!err(parse_test_request(text)).empty());
    // Gamepad settings change live like the graphics ones (config.json "gamepad" keys), alone or together.
    const auto pad_options=ok(parse_test_request(R"({"id":21,"cmd":"options","gamepad":{"type":"gamepad","model":"xbox"}})"));
    CHECK(pad_options.kind==TestRequestKind::options && pad_options.text.empty() && pad_options.gamepad==R"({"type":"gamepad","model":"xbox"})");
    const auto both_options=ok(parse_test_request(R"({"id":22,"cmd":"options","graphics":{"bloom":false},"gamepad":{"model":"nintendo"}})"));
    CHECK(both_options.text==R"({"bloom":false})" && both_options.gamepad==R"({"model":"nintendo"})");
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"options","gamepad":7})")).empty());
    // Rejections change nothing and name the problem.
    CHECK(!err(parse_test_request("not json")).empty());
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"input","kpad":{"x":2,"y":0,"buttons":0}})")).empty());     // out of range
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"input","kpad":{"x":0,"y":0,"buttons":1}})")).empty());     // button outside 0x1C10
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"input","hold_frames":601,"kpad":{"x":0,"y":0,"buttons":0}})")).empty());
    CHECK(ok(parse_test_request(R"({"id":9,"cmd":"speed","factor":32})")).factor==32);
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"speed","factor":33})")).empty());
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"state","extra":1})")).empty());                          // unknown field
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"input","key":{"code":0,"down":true}})")).empty());       // key code 1..511
    CHECK(!err(parse_test_request(R"({"id":1,"cmd":"input","kpad":{"x":0,"y":0,"buttons":0},"key":{"code":4,"down":true}})")).empty()); // two devices
    CHECK(err(parse_test_request(std::string(4097,' '))).find("long")!=std::string::npos);
    CHECK(std::get<TestParseError>(parse_test_request(R"({"id":5,"cmd":"nope"})")).id==5);              // id kept for the reply
    CHECK(test_reply_ok(3)==R"({"id":3,"ok":true})");
    CHECK(test_reply_error(3,"bad \"x\"")==R"({"id":3,"error":"bad \"x\""})");
    CHECK(test_reply_ok(4,R"("path":"a")")==R"({"id":4,"ok":true,"path":"a"})");
    return 0;
}
