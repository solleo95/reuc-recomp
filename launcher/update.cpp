#include "update.h"
#include "windows_path.h"
#include <objbase.h>
#include <cstdio>
// Windows part of UpdateController: the update helper process. The state machine is in update_common.cpp.
namespace uc::launcher {
using namespace update;
namespace {
void cleanup(const fs::path& operation) {
    auto status=remove_owned_tree(operation);
    if(failed(status)) std::fprintf(stderr,"[update] Cleanup deferred: %s\n",std::get<Error>(status).detail.c_str());
}
}
UpdateController::UpdateController(fs::path root,std::unique_ptr<Transport> transport,Helper helper):
    root_(uc::resolve_folder(root)),transport_(std::move(transport)),helper_(std::move(helper)) {   // behind a junction, a mounted drive or a network path: the real folder
    if(!helper_) helper_=[](const fs::path& root,const Package& package){return start_update_helper(root,package);};
}
void UpdateController::discard(Value& value) {
    if(auto* prepared=std::get_if<Prepared>(&value)) {
        if(prepared->helper.process.valid()) {
            auto process=prepared->helper.process.get();
            if(WaitForSingleObject(process,0)!=WAIT_OBJECT_0 && !TerminateProcess(process,2)) {
                std::fprintf(stderr,"[update] Cannot stop cancelled helper: Windows %lu\n",GetLastError());return;
            }
            if(WaitForSingleObject(process,10000)!=WAIT_OBJECT_0) {
                std::fprintf(stderr,"[update] Cancelled helper is still running; files retained\n");return;
            }
            prepared->helper.process=Handle{};
        }
        cleanup(prepared->operation);
    }
}
UpdateController::Value UpdateController::prepare(Release release,std::stop_token stop) {
    GUID guid{};wchar_t value[40]{};
    if(FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid,value,40)) return Error{ErrorCode::io,"Cannot create update operation ID"};
    fs::path operation=root_/".uc-update"/std::wstring(value+1,36);
    auto parent=lock_parent(root_/".uc-update",std::string(operation.filename().string()));
    if(failed(parent)) return std::get<Error>(parent);
    if(!CreateDirectoryW(operation.c_str(),nullptr)) return file_error("Create update operation");
    struct Staging {fs::path path;bool retained=false;~Staging(){if(!retained) cleanup(path);}} staging{operation};
    auto status=download_release(*transport_,release,operation/"package.zip",[this](uint64_t got,uint64_t){bytes_.store(got);},stop);
    if(failed(status)) return std::get<Error>(status);
    verifying_.store(true);
    auto package=prepare_package(operation/"package.zip",release,operation/"stage",stop);
    if(failed(package)) return std::get<Error>(package);
    if(stop.stop_requested()) return Error{ErrorCode::cancelled,"Update cancelled"};
    auto helper=helper_(root_,std::get<Package>(package));
    if(failed(helper)) return std::get<Error>(helper);
    staging.retained=true;
    return Prepared{std::get<HelperProcess>(std::move(helper)),operation};
}
}
