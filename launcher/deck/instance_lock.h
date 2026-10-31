#pragma once
#include <filesystem>
#include <utility>
namespace uc::deck {
// One launcher per install folder: an flock on <install>/.uc-launcher.lock for the launcher's lifetime. Two launchers at once would each run the start-up
// recovery (rolling back or deleting the other's update in progress) and could start two updates.
class InstanceLock {
public:
    enum class State { held, busy, unavailable };   // unavailable: the lock file cannot be created (read-only folder): the launcher goes on without exclusion
    static InstanceLock acquire(const std::filesystem::path& install);
    InstanceLock(InstanceLock&& other) noexcept : fd_(std::exchange(other.fd_, -1)), state_(other.state_) {}
    InstanceLock& operator=(InstanceLock&& other) noexcept;
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;
    ~InstanceLock();
    State state() const { return state_; }
    bool close_on_exec() const;   // the descriptor does not survive an exec: the game and the restarted launcher start without it
private:
    InstanceLock(int fd, State state) : fd_(fd), state_(state) {}
    int fd_;
    State state_;
};
}
