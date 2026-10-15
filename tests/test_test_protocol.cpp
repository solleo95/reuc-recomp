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
