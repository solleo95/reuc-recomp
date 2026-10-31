#pragma once
#include "download.h"
#ifdef _WIN32
#include "update_process.h"
#else
#include "transaction.h"
#endif
#include <atomic>
#include <future>
#include <thread>
namespace uc::launcher {
enum class UpdateState {disabled,checking,available,downloading,verifying,ready_to_close,failed};
class UpdateController {
public:
#ifdef _WIN32
    using Helper=std::function<update::Result<update::HelperProcess>(const std::filesystem::path&,const update::Package&)>;
    explicit UpdateController(std::filesystem::path root,
        std::unique_ptr<update::Transport> transport=std::make_unique<update::WinHttpTransport>(),Helper helper={});
#else
    // Linux: no helper process. The worker downloads, verifies, installs through the journaled transaction and leaves it awaiting the new
    // launcher's confirmation (see recover_pending_update / restart_after_update below).
    explicit UpdateController(std::filesystem::path root,
        std::unique_ptr<update::Transport> transport=std::make_unique<update::CurlTransport>());
    const std::filesystem::path& applied_operation() const {return prepared_->operation;}   // valid once close_requested()
#endif
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
#ifdef _WIN32
    struct Prepared {update::HelperProcess helper;std::filesystem::path operation;};
#else
    struct Prepared {std::filesystem::path operation;};
#endif
    using Value=std::variant<std::optional<update::Release>,Prepared,update::Error>;
    struct Completion {uint64_t generation;Value value;};
    void launch(std::function<Value(std::stop_token)> work);
    void discard(Value& result);
    Value prepare(update::Release release,std::stop_token stop);
    std::filesystem::path root_;
    std::unique_ptr<update::Transport> transport_;
#ifdef _WIN32
    Helper helper_;
#endif
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
#ifndef _WIN32
// Call first at start-up. Rolls back every update left pending (a crash or power cut before the new launcher confirmed), except the one the
// previous launcher named in UC_UPDATE_OPERATION when it re-executed itself: that operation is returned, and the caller commits it with
// update::finish_install() once its window is up. A rollback that fails leaves the files for the next start; `problem` says why (empty when all went well),
// for the launcher to show: a half-updated install must not be left unannounced.
struct PendingRecovery { std::optional<std::filesystem::path> confirm; std::string problem; };
PendingRecovery recover_pending_update(const std::filesystem::path& install);
// Replaces this process with the launcher file at `launcher` (the one the update just installed: /proc/self/exe would still name the replaced, now unlinked,
// inode); UC_UPDATE_OPERATION carries the operation to confirm. Returns only if exec fails.
void restart_launcher(const std::filesystem::path& launcher,const std::string& operation,char** argv);
#endif
}
