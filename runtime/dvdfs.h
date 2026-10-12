#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace uc {

class DvdFs {
public:
    bool open_root(const std::filesystem::path& root);
    int entry_of(std::string path) const;                 // -1 if absent; 0 is the root directory
    bool is_dir(int e) const { return valid(e) && e_[size_t(e)].dir; }
    uint32_t length(int e) const { return valid(e) ? e_[size_t(e)].size : 0; }
    int64_t read(int entry, void* dst, uint32_t len, uint32_t offset) const;   // bytes read (short at EOF), -1 on a bad entry
    size_t entry_count() const { return e_.size(); }
    std::string path_of(int e) const { return valid(e) ? e_[size_t(e)].rel : std::string(); }
private:
    struct Entry { std::string rel; std::filesystem::path full; bool dir; uint32_t size; };
    bool valid(int e) const { return e >= 0 && size_t(e) < e_.size(); }
    void walk(const std::filesystem::path& dir, const std::string& rel);
    std::vector<Entry> e_;
    std::map<std::string, int> index_;   // lower-case relative path -> entry
};

struct DvdStats { uint64_t opens = 0, reads = 0, bytes = 0; };
DvdStats dvd_stats();
void dvd_log(bool on);          // print "[dvd] open <path>" lines to stderr
void dvd_hle_init(DvdFs& fs);   // defined in hle/dvd.cpp
void dvd_hle_shutdown();       // call after guests stop, while their scheduler and file tree remain alive

class DvdSession {
public:
    explicit DvdSession(DvdFs& fs) { dvd_hle_init(fs); }
    ~DvdSession() { dvd_hle_shutdown(); }
    DvdSession(const DvdSession&) = delete;
    DvdSession& operator=(const DvdSession&) = delete;
};

}  // namespace uc
