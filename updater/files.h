#pragma once
#include "release.h"
#include <windows.h>
#include <filesystem>
#include <utility>
namespace uc::update {
namespace fs = std::filesystem;
class Handle {
    HANDLE value_ = INVALID_HANDLE_VALUE;
public:
    Handle() = default;
    explicit Handle(HANDLE value):value_(value) {}
    ~Handle() { if(valid()) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept:value_(std::exchange(other.value_,INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if(this!=&other) { if(valid()) CloseHandle(value_); value_=std::exchange(other.value_,INVALID_HANDLE_VALUE); }
        return *this;
    }
    bool valid() const { return value_!=INVALID_HANDLE_VALUE && value_!=nullptr; }
    HANDLE get() const { return value_; }
};
struct ParentLock { fs::path target; std::vector<Handle> directories; };
Error file_error(std::string action, DWORD code = GetLastError());
fs::path utf8_path(std::string_view text);
Result<ParentLock> lock_parent(const fs::path& root, std::string_view relative, bool create = false);
Result<Handle> open_read(const fs::path& path);
Result<std::string> read_text(const fs::path& path, uint64_t limit);
Result<std::string> sha256_handle(HANDLE handle);
Status rename_sibling(HANDLE file, std::wstring_view filename, bool replace = false);
Status remove_owned_tree(const fs::path& root);
}
