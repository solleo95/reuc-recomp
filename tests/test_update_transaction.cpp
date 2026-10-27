#include "check.h"
#include "transaction.h"
#include "files.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <map>
#include <cstdio>
using namespace uc::update;
using Json=nlohmann::json;
static unsigned checkpoints=0, crash_at=0;
static std::string fail_on;
namespace uc::update::detail {
Status checkpoint(std::string_view name) {
    ++checkpoints;
    if(crash_at && checkpoints==crash_at) ExitProcess(99);
    if(name==fail_on) return Error{ErrorCode::no_space,"injected disk full"};
    return std::monostate{};
}
}
template<class T> T ok(Result<T> value) {
    if(failed(value)) std::fprintf(stderr,"%s\n",std::get<Error>(value).detail.c_str());
    CHECK(!failed(value));return std::get<T>(std::move(value));
}
void pass(Status status) { if(failed(status)) std::fprintf(stderr,"%s\n",std::get<Error>(status).detail.c_str());CHECK(!failed(status)); }
void write(const fs::path& path,std::string_view data) {
    fs::create_directories(path.parent_path());std::ofstream f(path,std::ios::binary);f.write(data.data(),data.size());CHECK(f.good());
}
std::string read(const fs::path& path) {std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
Json make_manifest(const fs::path& root,std::string version,std::vector<std::string> names) {
    Json files=Json::array();
    for(const auto& name:names) files.push_back({{"path",name},{"size",fs::file_size(root/utf8_path(name))},
        {"sha256",ok(sha256_file(root/utf8_path(name)))}});
    return {{"format",1},{"version",version},{"files",files},{"helper_files",Json::array({"uc_updater.exe"})}};
}
std::map<std::string,std::string> tree(const fs::path& root) {
    std::map<std::string,std::string> result;
    for(auto it=fs::recursive_directory_iterator(root);it!=fs::recursive_directory_iterator();++it) {
        if(it->is_directory() && it->path().filename()==".uc-update") {it.disable_recursion_pending();continue;}
        if(it->is_regular_file()) result[it->path().lexically_relative(root).generic_string()]=read(it->path());
    }
    return result;
}
struct Fixture {
    fs::path root,operation,stage;
    Package package;
    Fixture() {
        static unsigned serial=0;
        root=fs::temp_directory_path()/("uc-transaction-"+std::to_string(GetCurrentProcessId())+"-"+
            std::to_string(GetTickCount64())+"-"+std::to_string(++serial));
        operation=root/".uc-update"/"11111111-1111-4111-8111-111111111111";stage=operation/"stage";
        fs::create_directories(stage);
        for(auto name:{"uc.exe","uc_launcher.exe","uc_updater.exe"}) {write(root/name,"OLD");write(stage/name,"NEW");}
        write(root/"obsolete.txt","OLD");write(stage/"added.txt","NEW");
        write(root/"saves"/"user.dat","USER");write(root/"mods"/"local.lua","USER");write(root/"config.json","USER");
        write(root/"data"/"pipeline_cache.bin","CACHE");write(root/"unknown.txt","UNKNOWN");
        write(root/"release.json",make_manifest(root,"0.0.1",{"uc.exe","uc_launcher.exe","uc_updater.exe","obsolete.txt"}).dump());
        write(stage/"release.json",make_manifest(stage,"0.0.2",{"uc.exe","uc_launcher.exe","uc_updater.exe","added.txt"}).dump());
        package={stage,ok(parse_manifest(read(stage/"release.json")))};
    }
    ~Fixture() {std::error_code ec;fs::remove_all(root,ec);if(ec) std::fprintf(stderr,"cleanup: %s\n",ec.message().c_str());}
};
unsigned child(const fs::path& root,unsigned crash,bool recover=false) {
    wchar_t exe[32768]{};CHECK(GetModuleFileNameW(nullptr,exe,32768));
    auto command=L"\""+std::wstring(exe)+(recover?L"\" --crash-recover \"":L"\" --crash \"")+root.wstring()+L"\" "+std::to_wstring(crash);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION pi{};
    CHECK(CreateProcessW(exe,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&pi));
    Handle process(pi.hProcess),thread(pi.hThread);CHECK(WaitForSingleObject(process.get(),30000)==WAIT_OBJECT_0);
    DWORD exit=0;CHECK(GetExitCodeProcess(process.get(),&exit));return exit;
}
int wmain(int argc,wchar_t** argv) {
    if(argc==4 && (std::wstring_view(argv[1])==L"--crash" || std::wstring_view(argv[1])==L"--crash-recover")) {
        fs::path root=argv[2];crash_at=std::stoul(argv[3]);
        auto stage=root/".uc-update"/"11111111-1111-4111-8111-111111111111"/"stage";
        if(std::wstring_view(argv[1])==L"--crash-recover") {pass(recover_install(root,stage.parent_path()));return 0;}
        Package package{stage,ok(parse_manifest(read(stage/"release.json")))};
        pass(apply_install(ok(plan_install(root,package))));return 0;
    }
    CHECK(argc==1);
    unsigned checkpoint_count=0,recovery_checkpoints=0;
    {
        Fixture f;auto before=tree(f.root);auto plan=ok(plan_install(f.root,f.package));checkpoints=0;
        pass(apply_install(plan));checkpoint_count=checkpoints;
        CHECK(read(f.root/"uc.exe")=="NEW");CHECK(read(f.root/"added.txt")=="NEW");CHECK(!fs::exists(f.root/"obsolete.txt"));
        CHECK(read(f.root/"saves"/"user.dat")=="USER");CHECK(read(f.root/"data"/"pipeline_cache.bin")=="CACHE");
        checkpoints=0;pass(recover_install(f.root,f.operation));recovery_checkpoints=checkpoints;CHECK(tree(f.root)==before);
        pass(recover_install(f.root,f.operation));CHECK(tree(f.root)==before);
    }
    {
        Fixture f;auto plan=ok(plan_install(f.root,f.package));pass(apply_install(plan));
        pass(finish_install(f.operation));CHECK(!fs::exists(f.operation/"backup"));auto committed=tree(f.root);
        pass(recover_install(f.root,f.operation));CHECK(tree(f.root)==committed);
    }
    {
        Fixture f;write(f.root/"added.txt","USER");CHECK(failed(plan_install(f.root,f.package)));
        CHECK(read(f.root/"added.txt")=="USER");
    }
    {
        Fixture f;write(f.root/"added.txt","NEW");pass(apply_install(ok(plan_install(f.root,f.package))));
        CHECK(read(f.root/"added.txt")=="NEW");CHECK(read(f.root/"uc.exe")=="NEW");
        pass(recover_install(f.root,f.operation));CHECK(read(f.root/"added.txt")=="NEW");
    }
    {
        Fixture f;auto j=Json::parse(read(f.root/"release.json"));j["files"][0]["path"]="config.json";
        write(f.root/"release.json",j.dump());CHECK(failed(plan_install(f.root,f.package)));
    }
    {
        Fixture f;fs::remove(f.root/"uc.exe");auto before=tree(f.root);
        pass(apply_install(ok(plan_install(f.root,f.package))));pass(recover_install(f.root,f.operation));CHECK(tree(f.root)==before);
    }
    {
        Fixture f;fs::remove(f.root/"uc.exe");fs::create_directory(f.root/"uc.exe");
        CHECK(failed(plan_install(f.root,f.package)));
    }
    {
        Fixture f;auto before=tree(f.root);auto plan=ok(plan_install(f.root,f.package));
        Handle locked(CreateFileW((f.root/"uc.exe").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        CHECK(locked.valid());CHECK(failed(apply_install(plan)));CHECK(tree(f.root)==before);
    }
    {
        Fixture f;auto before=tree(f.root);auto plan=ok(plan_install(f.root,f.package));fail_on="before_replace";
        CHECK(failed(apply_install(plan)));fail_on.clear();pass(recover_install(f.root,f.operation));CHECK(tree(f.root)==before);
    }
    {
        Fixture f;auto before=tree(f.root);pass(apply_install(ok(plan_install(f.root,f.package))));
        Handle locked(CreateFileW((f.root/"uc.exe").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        CHECK(failed(recover_install(f.root,f.operation)));CHECK(fs::exists(f.operation/"backup"));
        locked=Handle{};pass(recover_install(f.root,f.operation));CHECK(tree(f.root)==before);
    }
    {
        Fixture f;auto plan=ok(plan_install(f.root,f.package));write(f.stage/"uc.exe","TAMPERED");
        CHECK(failed(apply_install(plan)));CHECK(read(f.root/"uc.exe")=="OLD");
    }
    {
        Fixture f;auto before=tree(f.root);pass(apply_install(ok(plan_install(f.root,f.package))));
        auto header=read(f.operation/"journal.json");auto j=Json::parse(header);j["operations"][0]["path"]="../outside";
        write(f.operation/"journal.json",j.dump());CHECK(failed(recover_install(f.root,f.operation)));
        write(f.operation/"journal.json",header);pass(recover_install(f.root,f.operation));CHECK(tree(f.root)==before);
    }
    CHECK(checkpoint_count>5);
    {
        Fixture f;auto before=tree(f.root);pass(apply_install(ok(plan_install(f.root,f.package))));
        {std::ofstream log(f.operation/"journal.log",std::ios::app);log<<"{\"event\":\"rest";}
        pass(recover_install(f.root,f.operation));CHECK(tree(f.root)==before);
        pass(recover_install(f.root,f.operation));CHECK(tree(f.root)==before);
    }
    {
        Fixture f;auto before=tree(f.root);auto plan=ok(plan_install(f.root,f.package));
        write(f.root/(".uc-new-11111111-1111-4111-8111-111111111111-0"),"USER TEMP");
        before=tree(f.root);CHECK(failed(apply_install(plan)));CHECK(tree(f.root)==before);
        pass(recover_install(f.root,f.operation));CHECK(tree(f.root)==before);
    }
    {
        Fixture f;pass(apply_install(ok(plan_install(f.root,f.package))));
        auto temporary=f.root/".uc-new-11111111-1111-4111-8111-111111111111-0";
        write(temporary,"USER FILE AFTER CRASH");
        pass(recover_install(f.root,f.operation));
        CHECK(fs::exists(temporary));CHECK(read(temporary)=="USER FILE AFTER CRASH");
        CHECK(read(f.root/"uc.exe")=="OLD");
    }
    for(unsigned checkpoint=1;checkpoint<=checkpoint_count;++checkpoint) {
        Fixture f;auto before=tree(f.root);CHECK(child(f.root,checkpoint)==99);
        pass(recover_install(f.root,f.operation));CHECK(tree(f.root)==before);
        pass(recover_install(f.root,f.operation));CHECK(tree(f.root)==before);
    }
    CHECK(recovery_checkpoints>0);
    for(unsigned checkpoint=1;checkpoint<=recovery_checkpoints;++checkpoint) {
        Fixture f;auto before=tree(f.root);pass(apply_install(ok(plan_install(f.root,f.package))));
        CHECK(child(f.root,checkpoint,true)==99);
        pass(recover_install(f.root,f.operation));CHECK(tree(f.root)==before);
        pass(recover_install(f.root,f.operation));CHECK(tree(f.root)==before);
    }
    std::printf("update transaction: rollback, preservation and %u crash checkpoints passed\n",checkpoint_count);
}
