#include "update.h"
#include "posix/files_posix.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sys/stat.h>
#include <unistd.h>
// Linux part of UpdateController. There is no helper process: a running executable can be replaced by rename(2), so the worker installs the
// package itself through the journaled transaction (updater/posix/transaction_posix.cpp) and leaves the operation "awaiting startup".
// The launcher then re-executes itself; the new process commits the operation once its window is up, and a start that finds an
// unconfirmed operation (crash, power cut, a new launcher that never came up) rolls it back.
namespace uc::launcher {
using namespace update;
namespace {
constexpr const char* kEnv="UC_UPDATE_OPERATION";
void cleanup(const fs::path& operation) {
    auto status=remove_owned_tree(operation);
    if(failed(status)) std::fprintf(stderr,"[update] Cleanup deferred: %s\n",std::get<Error>(status).detail.c_str());
}
std::string new_operation_id() {   // an RFC 4122 version-4 UUID, the form transaction_posix.cpp accepts
    std::random_device device;
    std::uniform_int_distribution<unsigned> digit(0,15);
    static constexpr char hex[]="0123456789abcdef";
    std::string id(36,'-');
    for(size_t i=0;i<id.size();++i) {
        if(i==8||i==13||i==18||i==23) continue;
        id[i]=i==14?'4':i==19?hex[8+digit(device)%4]:hex[digit(device)];
    }
    return id;
}
}
UpdateController::UpdateController(fs::path root,std::unique_ptr<Transport> transport):
    root_(std::move(root)),transport_(std::move(transport)) {}
void UpdateController::discard(Value& value) {
    if(auto* prepared=std::get_if<Prepared>(&value)) {
        // Installed but never handed to a new launcher (Cancel during "verifying", which includes the install, or the launcher closing): put the old version back,
        // as the next start would for an unconfirmed update, so that "cancel" means what it says and the update can be offered again.
        auto status=recover_install(root_,prepared->operation);
        if(failed(status)) std::fprintf(stderr,"[update] Cannot roll back abandoned update: %s\n",std::get<Error>(status).detail.c_str());
        else cleanup(prepared->operation);
    }
}
UpdateController::Value UpdateController::prepare(Release release,std::stop_token stop) {
    const fs::path updates=root_/".uc-update";
    if(::mkdir(updates.c_str(),0700)!=0 && errno!=EEXIST) return file_error("Create update folder");
    const fs::path operation=updates/new_operation_id();
    if(::mkdir(operation.c_str(),0700)!=0) return file_error("Create update operation");
    struct Staging {fs::path path;bool retained=false;~Staging(){if(!retained) cleanup(path);}} staging{operation};
    auto status=download_release(*transport_,release,operation/"package.zip",[this](uint64_t got,uint64_t){bytes_.store(got);},stop);
    if(failed(status)) return std::get<Error>(status);
    verifying_.store(true);
    auto package=prepare_package(operation/"package.zip",release,operation/"stage",stop);
    if(failed(package)) return std::get<Error>(package);
    if(stop.stop_requested()) return Error{ErrorCode::cancelled,"Update cancelled"};
    auto plan=plan_install(root_,std::get<Package>(package));
    if(failed(plan)) return std::get<Error>(plan);
    status=apply_install(std::get<InstallPlan>(plan));
    if(failed(status)) {   // roll back whatever was done; the staging is removed by the guard when the rollback worked
        auto recovered=recover_install(root_,operation);
        if(failed(recovered)) {staging.retained=true;std::get<Error>(status).detail+="; rollback failed: "+std::get<Error>(recovered).detail;}
        return std::get<Error>(status);
    }
    staging.retained=true;
    return Prepared{operation};
}
PendingRecovery recover_pending_update(const fs::path& install) {
    PendingRecovery result;
    std::error_code ec;
    const fs::path updates=fs::absolute(install)/".uc-update";
    if(!fs::is_directory(updates,ec)) return result;
    const char* expected=std::getenv(kEnv);
    const auto report=[&](const fs::path& operation,const std::string& what,const std::string& detail) {
        std::fprintf(stderr,"[update] %s %s: %s\n",what.c_str(),operation.filename().c_str(),detail.c_str());
        if(!result.problem.empty()) result.problem+="; ";
        result.problem+=what+" "+operation.filename().string()+": "+detail;
    };
    for(const auto& entry:fs::directory_iterator(updates,ec)) {
        if(!entry.is_directory(ec)) continue;
        const auto& operation=entry.path();
        auto pending=pending_install(fs::absolute(install),operation);
        if(failed(pending)) {report(operation,"Update operation",std::get<Error>(pending).detail);continue;}
        if(!std::get<bool>(pending)) {cleanup(operation);continue;}   // committed or rolled back earlier: only the leftovers go
        if(expected && operation.filename()==expected) {result.confirm=operation;continue;}
        auto status=recover_install(fs::absolute(install),operation);
        if(failed(status)) report(operation,"Rollback of update",std::get<Error>(status).detail);
        else cleanup(operation);
    }
    ::unsetenv(kEnv);
    return result;
}
void restart_launcher(const fs::path& launcher,const std::string& operation,char** argv) {
    ::setenv(kEnv,operation.c_str(),1);
    ::execv(launcher.c_str(),argv);
    std::fprintf(stderr,"[update] Cannot restart the launcher: %s\n",std::strerror(errno));
}
}
