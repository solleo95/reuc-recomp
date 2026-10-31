#include "instance_lock.h"
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
namespace uc::deck {
InstanceLock InstanceLock::acquire(const std::filesystem::path& install) {
    const auto path = install / ".uc-launcher.lock";
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return InstanceLock(-1, State::unavailable);
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const bool busy = errno == EWOULDBLOCK;
        ::close(fd);
        return InstanceLock(-1, busy ? State::busy : State::unavailable);
    }
    return InstanceLock(fd, State::held);
}
InstanceLock& InstanceLock::operator=(InstanceLock&& other) noexcept {
    if (this != &other) { if (fd_ >= 0) ::close(fd_); fd_ = std::exchange(other.fd_, -1); state_ = other.state_; }
    return *this;
}
InstanceLock::~InstanceLock() { if (fd_ >= 0) ::close(fd_); }   // closing releases the flock
bool InstanceLock::close_on_exec() const {
    if (fd_ < 0) return true;
    const int flags = ::fcntl(fd_, F_GETFD);
    return flags >= 0 && (flags & FD_CLOEXEC);
}
}
