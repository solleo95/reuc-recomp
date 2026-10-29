#include "update_process.h"
#include <fstream>
namespace uc::update::detail {
Status checkpoint(std::string_view name) {
    if(name=="replaced" && std::filesystem::exists(std::filesystem::current_path().parent_path().parent_path().parent_path()/"kill-helper")) ExitProcess(99);
    return std::monostate{};
}
}
int wmain(int argc,wchar_t** argv) {
    using namespace uc::update;
    auto root=std::filesystem::current_path();
    if(argc==5 && (std::wstring_view(argv[1])==L"--apply" || std::wstring_view(argv[1])==L"--recover")) {
        if(std::filesystem::exists(root.parent_path().parent_path().parent_path()/"never-ready")) Sleep(10000);
        auto status=run_update_helper(argv[2],reinterpret_cast<HANDLE>(std::stoull(argv[3])),
            reinterpret_cast<HANDLE>(std::stoull(argv[4])),std::wstring_view(argv[1])==L"--recover",std::chrono::milliseconds(3000));
        if(failed(status)) {std::fprintf(stderr,"%s\n",std::get<Error>(status).detail.c_str());return 9;}
        return 0;
    }
    if(argc==2 && (std::wstring_view(argv[1])==L"--driver" || std::wstring_view(argv[1])==L"--native-driver")) {
        auto session=uc::enter_client_session(root);
        if(std::holds_alternative<std::error_code>(session)) return 10;
        auto stage=root/".uc-update"/"11111111-1111-4111-8111-111111111111"/"stage";
        auto manifest=parse_manifest(std::get<std::string>(read_text(stage/"release.json",max_manifest_size)));
        auto helper=start_update_helper(root,Package{stage,std::get<Manifest>(manifest)},
            std::wstring_view(argv[1])==L"--native-driver"?std::chrono::milliseconds(30000):std::chrono::milliseconds(3000));
        if(failed(helper)) return 11;
        std::ofstream(root/"helper-pid")<<GetProcessId(std::get<HelperProcess>(helper).process.get());
        std::ofstream(root/"helper-ready")<<"ready";
        if(std::filesystem::exists(root/"hold-parent")) Sleep(5000);
        return 0;
    }
    if(argc==2 && std::wstring_view(argv[1])==L"--recover-driver") {
        auto result=resume_pending_update(root);return !failed(result) && std::get<bool>(result)?0:12;
    }
    auto parsed=uc::update::parse_resume_startup(argc,argv);
    if(uc::update::failed(parsed)) return 3;
    auto startup=std::get<std::optional<uc::update::ResumeStartup>>(std::move(parsed));
    if(!startup) {
        auto session=uc::enter_client_session(root);if(std::holds_alternative<std::error_code>(session)) return 4;
        std::ofstream(root/"old-restarted")<<GetCurrentProcessId();return 0;
    }
    auto session=uc::enter_client_session(root,startup->ticket);
    if(std::holds_alternative<std::error_code>(session)) return 5;
    if(std::filesystem::exists(root/"exit-before-ack")) return 6;
    if(std::filesystem::exists(root/"never-ack")) {Sleep(60000);return 7;}
    if(uc::update::failed(startup->confirm(std::chrono::seconds(5)))) return 8;
    std::ofstream(root/"child-committed")<<GetCurrentProcessId();
    return 0;
}
