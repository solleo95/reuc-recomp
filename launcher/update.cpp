#include "update.h"
#include <objbase.h>
#include <cstdio>
namespace uc::launcher {
using namespace update;
namespace {
void cleanup(const fs::path& operation) {
    auto status=remove_owned_tree(operation);
    if(failed(status)) std::fprintf(stderr,"[update] Cleanup deferred: %s\n",std::get<Error>(status).detail.c_str());
}
}
UpdateController::UpdateController(fs::path root,std::unique_ptr<Transport> transport,Helper helper):
    root_(std::move(root)),transport_(std::move(transport)),helper_(std::move(helper)) {
    if(!helper_) helper_=[](const fs::path& root,const Package& package){return start_update_helper(root,package);};
}
UpdateController::~UpdateController() {
    worker_.request_stop();if(worker_.joinable()) worker_.join();
    if(completion_.valid()) {auto result=completion_.get();discard(result.value);}
    if(prepared_ && !handed_off_) {Value value=std::move(*prepared_);discard(value);}
}
void UpdateController::launch(std::function<Value(std::stop_token)> work) {
    if(worker_.joinable()) worker_.join();
    std::promise<Completion> promise;completion_=promise.get_future();auto generation=++generation_;
    worker_=std::jthread([work=std::move(work),promise=std::move(promise),generation](std::stop_token stop) mutable {
        try {promise.set_value({generation,work(stop)});}
        catch(const std::exception& e) {promise.set_value({generation,Error{ErrorCode::io,e.what()}});}
    });
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
void UpdateController::start(std::optional<Manifest> installed) {
    if(started_) return;started_=true;installed_=std::move(installed);
    if(!installed_) return;
    state_=UpdateState::checking;
    launch([this,version=installed_->version](std::stop_token stop)->Value {
        auto result=check_release(*transport_,version,stop);
        if(failed(result)) return std::get<Error>(result);
        return std::get<std::optional<Release>>(std::move(result));
    });
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
void UpdateController::accept(bool game_running) {
    if(busy() || !release_ || (state_!=UpdateState::available && state_!=UpdateState::failed)) return;
    if(game_running) {error_=Error{ErrorCode::busy,"Close the game before updating"};return;}
    error_.reset();bytes_.store(0);verifying_.store(false);state_=UpdateState::downloading;
    launch([this,release=*release_](std::stop_token stop){return prepare(release,stop);});
}
void UpdateController::cancel() {
    ++generation_;worker_.request_stop();error_.reset();
    if(prepared_) {Value value=std::move(*prepared_);prepared_.reset();discard(value);}
    state_=release_?UpdateState::available:UpdateState::disabled;
}
void UpdateController::postpone() {cancel();state_=UpdateState::disabled;}
void UpdateController::poll() {
    if(state_==UpdateState::downloading && verifying_.load()) state_=UpdateState::verifying;
    if(!completion_.valid() || completion_.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return;
    auto result=completion_.get();if(worker_.joinable()) worker_.join();
    if(result.generation!=generation_) {discard(result.value);return;}
    if(auto* error=std::get_if<Error>(&result.value)) {
        error_=*error;
        if(state_==UpdateState::checking && error->code!=ErrorCode::invalid_release && error->code!=ErrorCode::invalid_version) {
            state_=UpdateState::disabled;std::fprintf(stderr,"[update] Check skipped: %s\n",error->detail.c_str());
        }
        else state_=UpdateState::failed;
    } else if(auto* release=std::get_if<std::optional<Release>>(&result.value)) {
        release_=std::move(*release);state_=release_?UpdateState::available:UpdateState::disabled;
    } else {prepared_=std::get<Prepared>(std::move(result.value));state_=UpdateState::ready_to_close;}
}
bool UpdateController::busy() const {return completion_.valid();}
bool UpdateController::blocks_game_launch() const {
    return (busy() && release_.has_value()) || state_==UpdateState::ready_to_close;
}
bool UpdateController::close_requested() const {return state_==UpdateState::ready_to_close;}
void UpdateController::handoff() {if(close_requested()) handed_off_=true;}
}
