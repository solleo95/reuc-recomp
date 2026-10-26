#include "check.h"
#include "package.h"
#include "files.h"
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <winioctl.h>
#include <fstream>
#include <cstdio>
#include <chrono>
using namespace uc::update;
namespace fs = std::filesystem;
using Json = nlohmann::json;
template<class T> T ok(Result<T> result) { if(failed(result)) std::fprintf(stderr,"%s\n",std::get<Error>(result).detail.c_str()); CHECK(!failed(result)); return std::get<T>(std::move(result)); }
std::string read(const fs::path& path) { std::ifstream f(path,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
void write(const fs::path& path, std::string_view bytes) { std::ofstream f(path,std::ios::binary); f.write(bytes.data(),bytes.size()); CHECK(f.good()); }
struct Temp {
    fs::path root = fs::temp_directory_path() / ("uc-update-test-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    Temp() { CHECK(fs::create_directory(root)); }
    ~Temp() { std::error_code ec; fs::remove_all(root,ec); if(ec) std::fprintf(stderr,"cleanup: %s\n",ec.message().c_str()); }
};
Json manifest() {
    std::ifstream f(std::string(UC_SOURCE_DIR)+"/tests/fixtures/update/release-v1.json"); Json j; f>>j; return j;
}
std::string archive(Json json, std::string extra={}, bool omit=false) {
    mz_zip_archive z{}; CHECK(mz_zip_writer_init_heap(&z,0,0));
    auto text=json.dump();
    CHECK(mz_zip_writer_add_mem(&z,"release.json",text.data(),text.size(),MZ_BEST_SPEED));
    for(auto name:{"uc.exe","uc_launcher.exe","uc_updater.exe"}) {
        if(omit && std::string_view(name)=="uc.exe") continue;
        CHECK(mz_zip_writer_add_mem(&z,name,"",0,MZ_BEST_SPEED));
    }
    if(!extra.empty()) CHECK(mz_zip_writer_add_mem(&z,extra.c_str(),"x",1,MZ_BEST_SPEED));
    void* data=nullptr; size_t size=0; CHECK(mz_zip_writer_finalize_heap_archive(&z,&data,&size));
    std::string bytes(static_cast<const char*>(data),size); mz_free(data); CHECK(mz_zip_writer_end(&z)); return bytes;
}
void change_central(std::string& bytes, unsigned offset, uint32_t value, unsigned count) {
    auto pos=bytes.find(std::string("PK\1\2",4)); CHECK(pos!=bytes.npos);
    for(unsigned i=0;i<count;++i) bytes[pos+offset+i]=static_cast<char>(value>>(8*i));
}
int wmain(int argc,wchar_t** argv) {
    Temp temp;
    if(argc==6 && std::wstring_view(argv[1])==L"--verify-release") {
        const std::wstring version=argv[3], hash=argv[4];
        Release rel{ok(parse_version(std::string(version.begin(),version.end()))),{}, {},
            std::string(hash.begin(),hash.end()),std::stoull(argv[5])};
        auto result=prepare_package(fs::path(argv[2]),rel,temp.root/"stage");
        if(failed(result)) {std::fprintf(stderr,"%s\n",std::get<Error>(result).detail.c_str()); return 1;}
        return 0;
    }
    CHECK(argc==1);
    write(temp.root/"empty","");
    CHECK(ok(sha256_file(temp.root/"empty")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    write(temp.root/"abc","abc");
    CHECK(ok(sha256_file(temp.root/"abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    unsigned serial=0;
    auto prepare=[&](std::string bytes, bool bad_hash=false, std::stop_token stop={}) {
        auto zip=temp.root/("archive-"+std::to_string(++serial)+".zip"); write(zip,bytes);
        Release rel{{0,0,1},{},{},bad_hash?std::string(64,'f'):ok(sha256_file(zip)),bytes.size()};
        return prepare_package(zip,rel,temp.root/("stage-"+std::to_string(serial)),stop);
    };
    auto good=ok(prepare(archive(manifest())));
    CHECK(fs::exists(good.root/"uc.exe"));
    CHECK(!failed(verify_package(good)));
    write(good.root/"uc.exe","changed"); CHECK(failed(verify_package(good)));
    CHECK(failed(prepare(archive(manifest()),true)));
    for(auto path:{"../outside.txt","A.dll","config.json","C:/evil","a:b","NUL.txt","a. ","a//b","a\\b","a/../b","uc.exe/child","UC.EXE"})
        CHECK(failed(prepare(archive(manifest(),path))));
    CHECK(!fs::exists(temp.root/"outside.txt"));
    CHECK(failed(prepare(archive(manifest(),{},true))));
    auto j=manifest(); j["version"]="0.0.2"; CHECK(failed(prepare(archive(j))));
    j=manifest(); j["files"][0]["sha256"]=std::string(64,'f'); CHECK(failed(prepare(archive(j))));
    j=manifest(); j["files"][0]["size"]=1; CHECK(failed(prepare(archive(j))));
    auto bytes=archive(manifest()); change_central(bytes,38,0120777u<<16,4); CHECK(failed(prepare(bytes)));
    bytes=archive(manifest()); change_central(bytes,8,1,2); CHECK(failed(prepare(bytes))); // encrypted
    bytes=archive(manifest()); change_central(bytes,10,99,2); CHECK(failed(prepare(bytes))); // method
    bytes=archive(manifest()); change_central(bytes,16,0xdeadbeef,4); CHECK(failed(prepare(bytes))); // CRC
    bytes=archive(manifest()); change_central(bytes,24,0xffffffff,4); CHECK(failed(prepare(bytes)));
    CHECK(failed(prepare(archive(manifest(),std::string(600,'a')+".dll"))));
    std::stop_source cancelled; cancelled.request_stop();
    CHECK(failed(prepare(archive(manifest()),false,cancelled.get_token())));
    for(const auto& entry:fs::directory_iterator(temp.root))
        if(entry.is_directory()) CHECK(entry.path()==good.root);
    fs::create_directory(temp.root/"install");
    {
        auto guard=ok(lock_parent(temp.root/"install","nested/file.bin",true));
        CHECK(!MoveFileExW((temp.root/"install"/"nested").c_str(),(temp.root/"moved").c_str(),0));
        CHECK(GetLastError()==ERROR_SHARING_VIOLATION || GetLastError()==ERROR_ACCESS_DENIED);
        Handle writer(CreateFileW((temp.root/"install"/"nested").c_str(),GENERIC_WRITE,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        CHECK(!writer.valid());
    }
    CHECK(!failed(validate_target_path(temp.root/"install","data/file.bin")));
    CHECK(failed(validate_target_path(temp.root/"install","../outside")));
    // Junctions need no symbolic-link privilege.
    auto link=temp.root/"install"/"junction";
    fs::create_directory(link);
    HANDLE h=CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    CHECK(h!=INVALID_HANDLE_VALUE);
    auto target=L"\\??\\"+fs::absolute(temp.root).wstring();
    struct Mount { DWORD tag; WORD length,reserved,sub_off,sub_len,print_off,print_len; wchar_t path[2048]; } mount{};
    mount.tag=IO_REPARSE_TAG_MOUNT_POINT; mount.sub_len=static_cast<WORD>(target.size()*2);
    mount.print_off=mount.sub_len+2; mount.length=8+mount.sub_len+4;
    std::copy(target.begin(),target.end(),mount.path); DWORD returned=0;
    CHECK(DeviceIoControl(h,FSCTL_SET_REPARSE_POINT,&mount,mount.length+8,nullptr,0,&returned,nullptr));
    CloseHandle(h);
    CHECK(failed(validate_target_path(temp.root/"install","junction/outside")));
    CHECK(failed(validate_target_path(link,"outside")));
    auto zip=temp.root/"good.zip"; auto good_bytes=archive(manifest()); write(zip,good_bytes);
    Release rel{{0,0,1},{},{},ok(sha256_file(zip)),good_bytes.size()};
    CHECK(failed(prepare_package(zip,rel,link/"stage")));
    // Remove only the link, never its target.
    CHECK(RemoveDirectoryW(link.c_str()));
    std::puts("update package: integrity, unsafe archives and reparse boundaries passed");
}
