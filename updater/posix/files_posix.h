#pragma once
#include "release.h"
#include <cerrno>
#include <filesystem>
#include <sys/types.h>
#include <utility>
// POSIX twin of updater/files.h: descriptors instead of HANDLEs, symlinks refused (O_NOFOLLOW) instead of reparse points.
namespace uc::update {
namespace fs = std::filesystem;
class Fd {
    int fd_ = -1;
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd();
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept;
    bool valid() const { return fd_ >= 0; }
    int get() const { return fd_; }
};
Error file_error(std::string action, int code = errno);   // ENOSPC -> no_space, EACCES/EPERM/EROFS -> permission, the rest -> io
fs::path utf8_path(std::string_view text);
Result<Fd> open_read(const fs::path& path);   // a regular file; a symlink or any other kind is refused
Result<std::string> read_text(const fs::path& path, uint64_t limit);
Result<std::string> sha256_fd(int fd);       // from the start of the file
Status remove_owned_tree(const fs::path& root);   // deletes symlinks themselves, never what they point to
Status replace_file(const fs::path& from, const fs::path& to);   // rename(2), then fsync of the target's directory
Status fsync_file(const fs::path& path);
// fchmod(fd, wanted). Where the file system keeps modes as a mount option (exFAT, FAT, NTFS: an SD card) fchmod fails with EPERM/ENOTSUP and cannot be made to
// work; then the mode the file already shows decides: an execute bit wanted and present, or not wanted, is fine; wanted and absent is an error.
Status set_file_mode(int fd, mode_t wanted);
}
