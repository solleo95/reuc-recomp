#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace uc {
class SaveFs {
public:
    struct Stats { int result; uint32_t length=0,position=0; };
    struct Directory { int result; std::vector<std::string> names; };
    struct Usage { int result; uint32_t blocks=0,entries=0; };
    explicit SaveFs(std::filesystem::path root);
    ~SaveFs();
    SaveFs(const SaveFs&)=delete; SaveFs& operator=(const SaveFs&)=delete;
    int initialize();
    // Moves the game to another save folder: any thread. It takes effect at the first path operation of the game thread while no save file is open.
    // `done` runs on the game thread right after the switch (not when the switch fails: it is logged).
    void request_root(std::filesystem::path root, std::function<void()> done = {});
    int create_dir(const std::string& path);
    int create_file(const std::string& path);
    int open(const std::string& path,uint32_t mode);
    int close(int fd);
    int read(int fd,std::span<uint8_t> bytes);
    int write(int fd,std::span<const uint8_t> bytes);
    int seek(int fd,int32_t offset,int32_t whence);
    int rename(const std::string& from,const std::string& to);
    int remove(const std::string& path);
    int attributes(const std::string& path,bool& directory);
    Stats stats(int fd);
    Directory read_dir(const std::string& path);
    Usage usage(const std::string& path);
private:
    struct Pin { std::filesystem::path path; std::shared_ptr<void> handle; };
    using Pins=std::vector<Pin>;
    struct File { HANDLE handle; uint32_t mode; Pins parents; };
    std::filesystem::path root_;
    std::map<int,File> files_;
    int next_fd_=100;
    uint64_t revision_=0;
    std::mutex pending_mutex_;
    std::optional<std::filesystem::path> pending_root_;
    std::function<void()> pending_done_;
    int adopting_depth_=0;  // game thread only
    void adopt_pending();   // game thread, at the start of a path operation
    static int pin(const std::filesystem::path& directory,Pins& pins);
    int resolve(const std::string& path,std::filesystem::path& host,Pins& pins) const;
    int backup(HANDLE source);
    int backup(const std::filesystem::path& path);
    std::filesystem::path revision(const wchar_t* directory,Pins& pins);
};
}
