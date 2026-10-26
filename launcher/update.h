#pragma once
#include "download.h"
#include "update_process.h"
#include <atomic>
#include <future>
#include <thread>
namespace uc::launcher {
enum class UpdateState {disabled,checking,available,downloading,verifying,ready_to_close,failed};
class UpdateController {
public:
    using Helper=std::function<update::Result<update::HelperProcess>(const std::filesystem::path&,const update::Package&)>;
    explicit UpdateController(std::filesystem::path root,
        std::unique_ptr<update::Transport> transport=std::make_unique<update::WinHttpTransport>(),Helper helper={});
    ~UpdateController();
    void start(std::optional<update::Manifest> installed);
    void accept(bool game_running=false);
    void postpone();
    void cancel();
    void poll();
    bool busy() const;
    bool blocks_game_launch() const;
    bool close_requested() const;
    void handoff();
    UpdateState state() const {return state_;}
    const std::optional<update::Release>& release() const {return release_;}
    const std::optional<update::Error>& error() const {return error_;}
    update::Version installed_version() const {return installed_->version;}
    uint64_t downloaded() const {return bytes_.load();}
private:
    struct Prepared {update::HelperProcess helper;std::filesystem::path operation;};
    using Value=std::variant<std::optional<update::Release>,Prepared,update::Error>;
    struct Completion {uint64_t generation;Value value;};
    void launch(std::function<Value(std::stop_token)> work);
    void discard(Value& result);
    Value prepare(update::Release release,std::stop_token stop);
    std::filesystem::path root_;
    std::unique_ptr<update::Transport> transport_;
    Helper helper_;
    std::optional<update::Manifest> installed_;
    std::optional<update::Release> release_;
    std::optional<update::Error> error_;
    std::optional<Prepared> prepared_;
    UpdateState state_=UpdateState::disabled;
    bool started_=false,handed_off_=false;
    uint64_t generation_=0;
    std::atomic<uint64_t> bytes_{0};
    std::atomic<bool> verifying_{false};
    std::future<Completion> completion_;
    std::jthread worker_;
};
}
