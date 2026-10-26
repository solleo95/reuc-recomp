#include "check.h"
#include "download.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <fstream>
#include <cstdio>
using namespace uc::update;
namespace fs=std::filesystem;
using Json=nlohmann::json;
std::string hash="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
std::string url="https://github.com/solleo95/reuc-recomp/releases/download/0.0.1/UC-Windows-0.0.1.zip";
struct Fake:Transport {
    unsigned status=200;
    std::string data="abc";
    bool fail=false;
    std::stop_source* cancel=nullptr;
    std::string body=Json{{"tag_name","0.0.1"},{"draft",false},{"prerelease",false},{"body","Notes"},
        {"assets",Json::array({{{"name","UC-Windows-0.0.1.zip"},{"state","uploaded"},
            {"size",3},{"digest","sha256:"+hash},{"browser_download_url",url}}})}}.dump();
    Result<HttpResponse> get_metadata(std::stop_token) override { return HttpResponse{status,body}; }
    Status download(const Release&,const DownloadSink& sink,std::stop_token) override {
        if(fail) return Error{ErrorCode::network,"timeout"};
        auto result=sink(std::span<const char>(data.data(),data.size()));
        if(cancel) cancel->request_stop();
        return result;
    }
};
template<class T> ErrorCode error(const Result<T>& result) {CHECK(failed(result));return std::get<Error>(result).code;}
std::string read(const fs::path& p) {std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main() {
    CHECK(allowed_download_url(url,false));
    CHECK(allowed_download_url("https://release-assets.githubusercontent.com/a?token=secret",true));
    CHECK(allowed_download_url("https://objects.githubusercontent.com/a",true));
    for(auto u:{"http://github.com/a","https://user@github.com/a","https://github.com:444/a",
        "https://github.com.evil/a","https://evil.test/a","https://github.com/a#fragment",
        "https://github.com\\@evil.test/a","https://github.com/a\nX:header"})
        CHECK(!allowed_download_url(u,true));
    CHECK(!allowed_download_url("https://release-assets.githubusercontent.com/a",false));
    Fake fake;
    auto offer=check_release(fake,{0,0,0}); CHECK(!failed(offer)); CHECK(std::get<std::optional<Release>>(offer).has_value());
    for(unsigned status:{404u,500u}) {fake.status=status;CHECK(error(check_release(fake,{0,0,0}))==ErrorCode::network);}
    for(unsigned status:{403u,429u}) {fake.status=status;CHECK(error(check_release(fake,{0,0,0}))==ErrorCode::rate_limited);}
    fake.status=200;
    auto original=fake.body;fake.body="{";CHECK(failed(check_release(fake,{0,0,0})));
    fake.body=std::string(max_metadata_size+1,' ');CHECK(failed(check_release(fake,{0,0,0})));fake.body=original;
    auto meta=Json::parse(fake.body);meta["assets"][0].erase("digest");fake.body=meta.dump();CHECK(failed(check_release(fake,{0,0,0})));fake.body=original;
    auto root=fs::temp_directory_path()/("uc-download-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    CHECK(fs::create_directory(root));
    auto out=root/"release.zip";
    Release release{{0,0,1},url,"Notes",hash,3};
    uint64_t observed=0;
    auto downloaded=download_release(fake,release,out,[&](uint64_t n,uint64_t total){CHECK(total==3);observed=n;});
    if(failed(downloaded)) std::fprintf(stderr,"%s\n",std::get<Error>(downloaded).detail.c_str());
    CHECK(!failed(downloaded));
    CHECK(read(out)=="abc"); CHECK(observed==3);
    CHECK(failed(download_release(fake,release,out))); CHECK(read(out)=="abc");
    fs::remove(out);
    for(auto data:{"ab","abcd","xyz"}) {
        fake.data=data;CHECK(error(download_release(fake,release,out))==ErrorCode::integrity);
        CHECK(!fs::exists(out));CHECK(!fs::exists(fs::path(out.wstring()+L".part")));
    }
    fake.data="abc";fake.fail=true;CHECK(error(download_release(fake,release,out))==ErrorCode::network);CHECK(!fs::exists(out));fake.fail=false;
    std::stop_source stop;fake.cancel=&stop;
    CHECK(error(download_release(fake,release,out,{},stop.get_token()))==ErrorCode::cancelled);
    CHECK(!fs::exists(out));fake.cancel=nullptr;
    CHECK(error(check_release(fake,{0,0,0},stop.get_token()))==ErrorCode::cancelled);
    auto part=fs::path(out.wstring()+L".part");
    {std::ofstream f(part);f<<"EXISTING";}
    CHECK(failed(download_release(fake,release,out)));CHECK(read(part)=="EXISTING");
    fs::remove(part);
    WinHttpTransport native;CHECK(error(native.get_metadata(stop.get_token()))==ErrorCode::cancelled);
    fs::remove(root); std::puts("update download: URL policy, failures, cancellation and publication passed");
}
