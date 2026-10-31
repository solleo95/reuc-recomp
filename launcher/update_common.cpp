#include "update.h"
#include <cstdio>
// The platform-independent part of UpdateController (the state machine); update.cpp (Windows) and update_posix.cpp (Linux) hold prepare/discard.
namespace uc::launcher {
using namespace update;
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
