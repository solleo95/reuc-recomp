#include "client_update_lock.h"
#include "check.h"
#include "update_process.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <windows.h>
#include <cstdio>
namespace fs=std::filesystem;
using namespace std::chrono_literals;
template<class T> T take(std::variant<T,std::error_code> result) {
    if(auto* error=std::get_if<std::error_code>(&result)) std::fprintf(stderr,"%s\n",error->message().c_str());
    CHECK(std::holds_alternative<T>(result));return std::get<T>(std::move(result));
}
template<class T> bool denied(const std::variant<T,std::error_code>& result) {return std::holds_alternative<std::error_code>(result);}
template<class T> T ok(uc::update::Result<T> result) {
    if(uc::update::failed(result)) std::fprintf(stderr,"%s\n",std::get<uc::update::Error>(result).detail.c_str());
    CHECK(!uc::update::failed(result));return std::get<T>(std::move(result));
}
void process_tests(const fs::path& base) {
    using namespace uc::update;
    wchar_t module[32768]{};CHECK(GetModuleFileNameW(nullptr,module,32768));
    auto child=fs::path(module).parent_path()/"update_test_child.exe";
    for(auto scenario:{"success","exit-before-ack","never-ack","missing-child"}) {
        auto root=base/scenario,operation=root/".uc-update"/"11111111-1111-4111-8111-111111111111",stage=operation/"stage";
        fs::create_directories(stage);
        for(const auto& folder:{root,stage}) {
            fs::copy_file(child,folder/"uc_launcher.exe");
            std::ofstream(folder/"uc.exe")<<(folder==root?"OLD":"NEW");
            std::ofstream(folder/"uc_updater.exe")<<"DUMMY";
            nlohmann::json files=nlohmann::json::array();
            for(auto name:{"uc.exe","uc_launcher.exe","uc_updater.exe"}) files.push_back({{"path",name},
                {"size",fs::file_size(folder/name)},{"sha256",ok(sha256_file(folder/name))}});
            nlohmann::json manifest={{"format",1},{"version",folder==root?"0.0.1":"0.0.2"},
                {"files",files},{"helper_files",nlohmann::json::array({"uc_updater.exe"})}};
            std::ofstream(folder/"release.json")<<manifest.dump();
        }
        auto guard=take(uc::reserve_client_install(root));CHECK(!guard.wait_until_unused(10ms));
        Package package{stage,ok(parse_manifest(ok(read_text(stage/"release.json",max_manifest_size))))};
        ok(apply_install(ok(plan_install(root,package))));
        if(std::string_view(scenario)=="missing-child") fs::remove(root/"uc_launcher.exe");
        else if(std::string_view(scenario)!="success") std::ofstream(root/scenario)<<"fixture";
        auto result=start_and_confirm(root,operation,guard,500ms);
        if(std::string_view(scenario)=="success") {
            ok(std::move(result));
            for(int i=0;i<100 && !fs::exists(root/"child-committed");++i) Sleep(10);
            CHECK(fs::exists(root/"child-committed"));CHECK(!fs::exists(operation/"backup"));
        } else {
            CHECK(failed(result));CHECK(!fs::exists(root/"child-committed"));
            CHECK(!guard.reacquire_for_rollback(500ms));ok(recover_install(root,operation));
            CHECK(ok(read_text(root/"uc.exe",100))=="OLD");
        }
        if(std::string_view(scenario)=="success") {auto next=take(uc::reserve_client_install(root));CHECK(!next.wait_until_unused(1000ms));}
        else CHECK(!guard.reacquire_for_rollback(1000ms));
    }
}
void helper_tests(const fs::path& base) {
    using namespace uc::update;
    wchar_t module[32768]{};CHECK(GetModuleFileNameW(nullptr,module,32768));
    auto child=fs::path(module).parent_path()/"update_test_child.exe";
    for(auto scenario:{"helper-success","exit-before-ack","hold-parent","kill-helper","broken-helper","never-ready"}) {
        auto root=base/scenario,operation=root/".uc-update"/"11111111-1111-4111-8111-111111111111",stage=operation/"stage";
        fs::create_directories(stage);
        for(const auto& folder:{root,stage}) {
            fs::copy_file(child,folder/"uc_launcher.exe");
            if(std::string_view(scenario)=="broken-helper") std::ofstream(folder/"uc_updater.exe")<<"INVALID EXE";
            else fs::copy_file(child,folder/"uc_updater.exe");
            std::ofstream(folder/"uc.exe")<<(folder==root?"OLD":"NEW");
            nlohmann::json files=nlohmann::json::array();
            for(auto name:{"uc.exe","uc_launcher.exe","uc_updater.exe"}) files.push_back({{"path",name},
                {"size",fs::file_size(folder/name)},{"sha256",ok(sha256_file(folder/name))}});
            nlohmann::json manifest={{"format",1},{"version",folder==root?"0.0.1":"0.0.2"},
                {"files",files},{"helper_files",nlohmann::json::array({"uc_updater.exe"})}};
            std::ofstream(folder/"release.json")<<manifest.dump();
        }
        if(std::string_view(scenario)!="helper-success") std::ofstream(root/scenario)<<"fixture";
        std::array<std::wstring,1> args{L"--driver"};
        auto driver=ok(create_child(child,root,args));CHECK(WaitForSingleObject(driver.get(),10000)==WAIT_OBJECT_0);
        DWORD code=0;CHECK(GetExitCodeProcess(driver.get(),&code));
        if(std::string_view(scenario)=="broken-helper" || std::string_view(scenario)=="never-ready") {
            CHECK(code==11);CHECK(ok(read_text(root/"uc.exe",100))=="OLD");CHECK(!fs::exists(operation/"journal.json"));
            CHECK(!ok(resume_pending_update(root)));CHECK(!fs::exists(operation));continue;
        }
        CHECK(code==0);CHECK(fs::exists(root/"helper-ready"));
        if(std::string_view(scenario)=="hold-parent") {
            CHECK(ok(read_text(root/"uc.exe",100))=="OLD");CHECK(!fs::exists(operation/"journal.json"));
            CHECK(!ok(resume_pending_update(root)));CHECK(!fs::exists(operation));continue;
        }
        if(std::string_view(scenario)=="kill-helper") {
            for(int i=0;i<200 && !fs::exists(operation/"journal.json");++i) Sleep(10);
            bool stopped=false;
            for(int i=0;i<200;++i) {
                auto guard=uc::reserve_client_install(root);
                if(!denied(guard)) {stopped=true;break;} Sleep(10);
            }
            CHECK(stopped);CHECK(denied(uc::enter_client_session(root)));CHECK(ok(pending_install(root,operation)));
            args[0]=L"--recover-driver";auto recovery=ok(create_child(child,root,args));
            CHECK(WaitForSingleObject(recovery.get(),10000)==WAIT_OBJECT_0);
            CHECK(GetExitCodeProcess(recovery.get(),&code));CHECK(code==0);
        }
        auto marker=root/(std::string_view(scenario)=="helper-success"?"child-committed":"old-restarted");
        for(int i=0;i<500 && !fs::exists(marker);++i) Sleep(10);
        CHECK(fs::exists(marker));CHECK(!ok(pending_install(root,operation)));
        CHECK(ok(read_text(root/"uc.exe",100))==(std::string_view(scenario)=="helper-success"?"NEW":"OLD"));
        auto guard=take(uc::reserve_client_install(root));CHECK(!guard.wait_until_unused(1000ms));
    }
}
void cleanup_tests(const fs::path& base) {
    using namespace uc::update;
    wchar_t module[32768]{};CHECK(GetModuleFileNameW(nullptr,module,32768));
    auto child=fs::path(module).parent_path()/"update_test_child.exe";
    bool before_journal_cleaned=false,locked_cleanup_retained=false,locked_cleanup_finished=false;
    for(auto scenario:{"before-journal","locked-recovery","pending-marker","locked-journal","locked-marker-temp"}) {
        auto root=base/scenario,operation=root/".uc-update"/"11111111-1111-4111-8111-111111111111",stage=operation/"stage";
        fs::create_directories(stage);
        for(const auto& folder:{root,stage}) {
            fs::copy_file(child,folder/"uc_launcher.exe");
            fs::copy_file(child,folder/"uc_updater.exe");
            std::ofstream(folder/"uc.exe")<<(folder==root?"OLD":"NEW");
            nlohmann::json files=nlohmann::json::array();
            for(auto name:{"uc.exe","uc_launcher.exe","uc_updater.exe"}) files.push_back({{"path",name},
                {"size",fs::file_size(folder/name)},{"sha256",ok(sha256_file(folder/name))}});
            nlohmann::json manifest={{"format",1},{"version",folder==root?"0.0.1":"0.0.2"},
                {"files",files},{"helper_files",nlohmann::json::array({"uc_updater.exe"})}};
            std::ofstream(folder/"release.json")<<manifest.dump();
        }
        fs::create_directories(operation/"recovery");
        fs::copy_file(child,operation/"recovery"/"uc_updater.exe");
        std::ofstream(operation/"release.zip")<<"download";
        if(std::string_view(scenario)=="before-journal") {
            {
                auto locked=ok(open_read(root/"uc.exe"));
                auto result=run_update_helper(operation,nullptr,nullptr,false,500ms);
                CHECK(failed(result));CHECK(!fs::exists(operation/"journal.json"));
                CHECK(ok(read_text(root/"uc.exe",100))=="OLD");
            }
            for(int i=0;i<100 && !fs::exists(root/"old-restarted");++i) Sleep(10);
            CHECK(!ok(resume_pending_update(root)));
            before_journal_cleaned=!fs::exists(operation);
        } else {
            Package package{stage,ok(parse_manifest(ok(read_text(stage/"release.json",max_manifest_size))))};
            ok(apply_install(ok(plan_install(root,package))));
            if(std::string_view(scenario)=="pending-marker") {
                std::ofstream(operation/"cleanup-ready")<<"11111111-1111-4111-8111-111111111111\n";
                CHECK(failed(resume_pending_update(root)));CHECK(ok(pending_install(root,operation)));
                CHECK(fs::exists(operation/"backup"));CHECK(fs::exists(stage/"uc.exe"));continue;
            }
            ok(finish_install(operation));
            std::ofstream(operation/"cleanup-ready.new")<<"partial";
            if(std::string_view(scenario)=="locked-marker-temp") {
                {
                    auto locked=ok(open_read(operation/"cleanup-ready.new"));
                    CHECK(!ok(resume_pending_update(root)));CHECK(!fs::exists(operation/"cleanup-ready"));
                    CHECK(fs::exists(operation/"journal.json"));CHECK(fs::exists(operation/"journal.log"));CHECK(fs::exists(stage));
                }
                CHECK(!ok(resume_pending_update(root)));CHECK(!fs::exists(operation));continue;
            }
            if(std::string_view(scenario)=="locked-journal") {
                {
                    auto locked=ok(open_read(operation/"journal.json"));
                    CHECK(!ok(resume_pending_update(root)));
                    CHECK(fs::exists(operation/"journal.json"));CHECK(fs::exists(operation/"journal.log"));
                    CHECK(fs::exists(operation/"cleanup-ready"));
                    CHECK(!fs::exists(stage));CHECK(!fs::exists(operation/"recovery"));CHECK(!fs::exists(operation/"release.zip"));
                }
                CHECK(!ok(resume_pending_update(root)));CHECK(!fs::exists(operation));continue;
            }
            {
                auto locked=ok(open_read(operation/"recovery"/"uc_updater.exe"));
                CHECK(!ok(resume_pending_update(root)));
                locked_cleanup_retained=fs::exists(operation/"cleanup-ready") && fs::exists(operation/"recovery"/"uc_updater.exe");
            }
            CHECK(!ok(resume_pending_update(root)));
            locked_cleanup_finished=!fs::exists(operation);
        }
    }
    auto untouched=base/"unmarked"/".uc-update"/"11111111-1111-4111-8111-111111111111";
    fs::create_directories(untouched/"stage");std::ofstream(untouched/"release.zip")<<"download";
    std::ofstream(untouched/"cleanup-ready.new")<<"partial";
    CHECK(!ok(resume_pending_update(untouched.parent_path().parent_path())));
    CHECK(fs::exists(untouched/"stage"));CHECK(fs::exists(untouched/"release.zip"));
    CHECK(ok(read_text(untouched/"cleanup-ready.new",100))=="partial");
    {auto session=take(uc::enter_client_session(untouched.parent_path().parent_path()));}
    std::ofstream(untouched/"cleanup-ready")<<"invalid";
    CHECK(failed(resume_pending_update(untouched.parent_path().parent_path())));
    CHECK(fs::exists(untouched/"stage"));CHECK(fs::exists(untouched/"release.zip"));
    fs::remove(untouched/"cleanup-ready");fs::create_directory(untouched/"cleanup-ready");
    CHECK(failed(resume_pending_update(untouched.parent_path().parent_path())));
    CHECK(fs::exists(untouched/"stage"));CHECK(fs::exists(untouched/"release.zip"));
    std::fprintf(stderr,"cleanup checks: before-journal=%d locked-retained=%d locked-finished=%d\n",
        before_journal_cleaned,locked_cleanup_retained,locked_cleanup_finished);
    CHECK(before_journal_cleaned);CHECK(locked_cleanup_retained);CHECK(locked_cleanup_finished);
}
int main() {
    const auto root=fs::temp_directory_path()/("uc-lock-"+std::to_string(GetCurrentProcessId()));
    fs::create_directories(root);
    {
        auto first=std::optional{take(uc::enter_client_session(root))};
        auto second=std::optional{take(uc::enter_client_session(root))};
        auto installer=take(uc::reserve_client_install(root));
        CHECK(denied(uc::reserve_client_install(root)));
        CHECK(denied(uc::enter_client_session(root)));
        CHECK(installer.wait_until_unused(10ms));
        first.reset();CHECK(installer.wait_until_unused(10ms));
        second.reset();CHECK(!installer.wait_until_unused(10ms));
        CHECK(!installer.allow_child_start());
        CHECK(denied(uc::enter_client_session(root,uc::ResumeTicket{0,"bad"})));
        const std::string id="11111111-1111-4111-8111-111111111111";
        auto channel=take(installer.resume_channel(id));
        CHECK(denied(uc::enter_client_session(root,uc::ResumeTicket{channel,"wrong"})));
        CloseHandle(reinterpret_cast<HANDLE>(channel));
        channel=take(installer.resume_channel(id));
        auto child=std::optional{take(uc::enter_client_session(root,uc::ResumeTicket{channel,id}))};
        CHECK(denied(uc::enter_client_session(root,uc::ResumeTicket{channel,id})));
        CloseHandle(reinterpret_cast<HANDLE>(channel));
        CHECK(installer.reacquire_for_rollback(10ms));
        child.reset();CHECK(!installer.reacquire_for_rollback(10ms));
    }
    {auto session=take(uc::enter_client_session(root));}
    process_tests(root);
    helper_tests(root/"helpers");
    cleanup_tests(root/"cleanup");
    fs::remove_all(root);
    std::puts("update process: session, installer and one-use ticket checks passed");
}
