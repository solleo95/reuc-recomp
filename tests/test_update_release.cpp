#include "check.h"
#include "release.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <cstdio>
using namespace uc::update;
using Json = nlohmann::json;
template<class T> const T& ok(const Result<T>& value) { CHECK(!failed(value)); return std::get<T>(value); }
const std::string hash(64, 'a');
Json manifest() {
    std::ifstream file(std::string(UC_SOURCE_DIR) + "/tests/fixtures/update/release-v1.json");
    CHECK(file.good()); Json value; file >> value; return value;
}
Json release() {
    return {{"tag_name","v0.0.1"},{"draft",false},{"prerelease",false},{"body","Changes"},
        {"assets",Json::array({{{"name","UC-Windows-0.0.1.zip"},{"state","uploaded"},
        {"size",100},{"digest","sha256:"+hash},
        {"browser_download_url","https://github.com/solleo95/reuc-recomp/releases/download/v0.0.1/UC-Windows-0.0.1.zip"}}})}};
}
int main() {
    CHECK((ok(parse_version("0.0.1")) == Version{0,0,1}));
    CHECK(ok(parse_version("0.0.9")) < ok(parse_version("0.0.10")));
    CHECK(ok(parse_version("v0.0.1", true)) == ok(parse_version("0.0.1")));
    CHECK(format_version({1,20,300}) == "1.20.300");
    for (auto text : {"", "01.0.0", "65536.0.0", "0.0", "0.0.1.2", "-1.0.0", "+1.0.0",
                      "0.0.1-beta", " 0.0.1", "0.0.1 ", "9999999999999.0.0", "v0.0.1"})
        CHECK(failed(parse_version(text)));
    for (auto path : {"config.json","CPU-PROFILE.JSON","GAME/DATA/sys/main.dol","saves/a","mods/a",
                      "textures/a","debug/a","logs/a",".uc-update/a","release.json","/x","../x","a/../x",
                      "C:/x","//host/x","a:b","a\\b","a//b","a/./b","a/","NUL.txt","con","a/COM1.dll",
                      "a/LPT9","a. ","a ","a\t","a\x7f","a~1.dll",
                      "config.json.write-lock","config.json.tmp-123","cpu-profile.json.calibrating",
                      "cpu-profile.json.tmp-123","nvngx_dlssnr.dll","cpu-samples.txt",
                      "data/pipeline_keys.bin","data/pipeline_cache.bin","data/pipeline_cache.tmp"})
        CHECK(failed(validate_managed_path(path)));
    CHECK(failed(validate_managed_path(std::string("a\0b",3))));
    CHECK(failed(validate_managed_path(std::string("a\xff",2))));
    CHECK(!failed(validate_managed_path("data/shaders/example.spv")));
    CHECK(!failed(validate_managed_path("assets/launcher/graphite-train.png")));
    CHECK(ok(path_key("Data/A.dll")) == ok(path_key("data/a.DLL")));
    CHECK(ok(path_key("\xd0\xa4.dll")) == ok(path_key("\xd1\x84.DLL")));
    auto good = manifest();
    CHECK((ok(parse_manifest(good.dump())).version == Version{0,0,1}));
    for (auto text : {"", "{", "[]", "null", "{\"format\":1}"}) CHECK(failed(parse_manifest(text)));
    auto bad = good; bad["format"]=2; CHECK(failed(parse_manifest(bad.dump())));
    bad=good; bad["version"]=3; CHECK(failed(parse_manifest(bad.dump())));
    bad=good; bad["files"][0]["size"]=-1; CHECK(failed(parse_manifest(bad.dump())));
    bad=good; bad["files"][0]["size"]=3.5; CHECK(failed(parse_manifest(bad.dump())));
    bad=good; bad["files"][0]["size"]=max_zip_size+1; CHECK(failed(parse_manifest(bad.dump())));
    bad=good; bad["files"][0]["sha256"]="bad"; CHECK(failed(parse_manifest(bad.dump())));
    bad=good; bad["files"].push_back(bad["files"][0]); CHECK(failed(parse_manifest(bad.dump())));
    bad=good; bad["files"][0]["path"]="UC_LAUNCHER.EXE"; CHECK(failed(parse_manifest(bad.dump())));
    bad=good; bad["files"][0]["path"]="release.json"; CHECK(failed(parse_manifest(bad.dump())));
    bad=good; bad["files"].erase(0); CHECK(failed(parse_manifest(bad.dump())));
    bad=good; bad["helper_files"]=Json::array(); CHECK(failed(parse_manifest(bad.dump())));
    bad=good; bad["helper_files"]=Json::array({"uc_updater.exe","../evil.dll"}); CHECK(failed(parse_manifest(bad.dump())));
    bad=good; bad["helper_files"]=Json::array({"uc_updater.exe","uc.exe"}); CHECK(failed(parse_manifest(bad.dump())));
    auto rel=release();
    CHECK(ok(select_release(rel.dump(),{0,0,0}))->sha256 == hash);
    CHECK(!ok(select_release(rel.dump(),{0,0,1})));
    CHECK(!ok(select_release(rel.dump(),{1,0,0})));
    auto r=rel; r["draft"]=true; CHECK(!ok(select_release(r.dump(),{0,0,0})));
    r=rel; r["prerelease"]=true; CHECK(!ok(select_release(r.dump(),{0,0,0})));
    r=rel; r["tag_name"]="v0.0.2"; CHECK(failed(select_release(r.dump(),{0,0,0})));
    r=rel; r["assets"][0].erase("digest"); CHECK(failed(select_release(r.dump(),{0,0,0})));
    r=rel; r["assets"][0]["digest"]="sha256:bad"; CHECK(failed(select_release(r.dump(),{0,0,0})));
    r=rel; r["assets"][0]["size"]=-1; CHECK(failed(select_release(r.dump(),{0,0,0})));
    r=rel; r["assets"][0]["state"]="new"; CHECK(failed(select_release(r.dump(),{0,0,0})));
    r=rel; r["assets"].push_back(r["assets"][0]); CHECK(failed(select_release(r.dump(),{0,0,0})));
    r=rel; r["draft"]="no"; CHECK(failed(select_release(r.dump(),{0,0,0})));
    r=rel; r["body"]=nullptr; CHECK(ok(select_release(r.dump(),{0,0,0}))->notes.empty());
    r=rel; r["assets"][0]["browser_download_url"]="https://evil.test/file.zip"; CHECK(failed(select_release(r.dump(),{0,0,0})));
    CHECK(failed(select_release("[]",{0,0,0})));
    CHECK(failed(select_release(std::string(max_metadata_size+1,' '),{0,0,0})));
    std::puts("update release: versions, ownership and metadata passed");
}
