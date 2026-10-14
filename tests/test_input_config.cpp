#include "check.h"
#include "input_config.h"
#include <fstream>
#include <chrono>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
using namespace uc;
template<class F> static void rejects(F f) {
    bool caught=false; try {f();} catch(const std::exception&) {caught=true;} CHECK(caught);
}
int main() {
    CHECK(parse_config("{}").input==default_bindings());
    auto config=parse_config(R"({"graphics":{"future":42},"input":{"version":1,"bindings":{"grenade":[],"fire":[{"kind":"key","code":9},{"kind":"mouse","code":3}]}}})");
    CHECK(config.input[size_t(Action::grenade)].empty());
    CHECK(config.input[size_t(Action::fire)].size()==2);
    auto roundtrip=parse_config(serialize_config(config));
    CHECK(roundtrip.input==config.input && roundtrip.properties["graphics"]["future"]==42);
    CHECK(parse_config(serialize_config(parse_config("{}"))).input==default_bindings());
    for(auto text:{R"({"input":{"version":2}})",R"({"input":{"version":1,"bindings":{"unknown":[]}}})",
        R"({"input":{"version":1,"version":1}})",R"({"x":{"a":1,"a":2}})",R"({"input":null})",
        R"({"input":{"version":1,"bindings":{"fire":[{"kind":"key","code":-1}]}}})",
        R"({"input":{"version":1,"bindings":{"fire":[{"kind":"key","code":9.0}]}}})",
        R"({"input":{"version":1,"bindings":{"fire":[{"kind":"key","code":512}]}}})",
        R"({"input":{"version":1,"bindings":{"fire":[{"kind":"key","code":9,"extra":1}]}}})",
        R"({"input":{"version":1,"bindings":{"fire":[{"kind":"interact_fire"}]}}})",
        R"({"input":{"version":1,"bindings":{"fire":[{"kind":"key","code":9},{"kind":"key","code":9}]}}})",
        R"({"input":{"version":1,"bindings":{"fire":null}}})"}) rejects([&]{parse_config(text);});
    rejects([]{parse_config("{\"future\":\""+std::string(1024*1024,'x')+"\"}");});
    rejects([]{parse_config("{\"future\":"+std::string(33,'[')+"0"+std::string(33,']')+"}");});
    CHECK(parse_config("{\"future\":"+std::string(30,'[')+"0"+std::string(30,']')+"}").input==default_bindings());
    auto oversized=nlohmann::ordered_json::parse(R"({"input":{"version":1,"bindings":{"fire":[]}}})");
    for(unsigned i=1;i<=65;++i)oversized["input"]["bindings"]["fire"].push_back({{"kind","key"},{"code",i}});
    rejects([&]{parse_config(oversized.dump());});
    auto path=std::filesystem::temp_directory_path()/std::filesystem::path(u"uc-config-тест")/
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())/"config.json";
    std::filesystem::create_directories(path.parent_path());
    auto fresh=load_config(path); CHECK(fresh.input==default_bindings());
    fresh.input[size_t(Action::knife)].clear(); save_config(path,fresh);
    auto saved=load_config(path); CHECK(saved.input==fresh.input);
    auto stale=saved; saved.properties["unrelated"]=123; save_config(path,saved);
    rejects([&]{save_config(path,stale);});
    CHECK(load_config(path).properties["unrelated"]==123);
    auto invalid=saved; invalid.input[size_t(Action::fire)]={{BindingKind::physical,{PhysicalKind::key,0},{}}};
    rejects([&]{save_config(path,invalid);}); CHECK(load_config(path).input==saved.input);
    auto lock_path=path; lock_path+=L".write-lock";
    std::ofstream writer_lock(lock_path); writer_lock<<"other writer"; writer_lock.close();
    rejects([&]{save_config(path,saved);}); CHECK(load_config(path).properties["unrelated"]==123);
    auto modifiers=parse_config(R"({"input":{"version":1,"bindings":{"fire":[{"kind":"mouse","code":3,"modifiers":[{"kind":"key","code":224}]}]}}})");
    CHECK(parse_config(serialize_config(modifiers)).input==modifiers.input);
    auto rollback_path=path.parent_path()/"rollback.json";
    auto rollback=load_config(rollback_path);save_config(rollback_path,rollback);
    auto reader=CreateFileW(rollback_path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    CHECK(reader!=INVALID_HANDLE_VALUE);
    rollback.properties["new_value"]=7;
    rejects([&]{save_config(rollback_path,rollback);});
    CHECK(!load_config(rollback_path).properties.contains("new_value"));
    CloseHandle(reader);save_config(rollback_path,rollback);
    CHECK(load_config(rollback_path).properties["new_value"]==7);
    std::ofstream(path)<<"{bad";
    rejects([&]{load_config(path);}); rejects([&]{save_config(path,saved);});
    std::ifstream original(path); std::string bytes{std::istreambuf_iterator<char>(original),{}}; CHECK(bytes=="{bad");
    return 0;
}
