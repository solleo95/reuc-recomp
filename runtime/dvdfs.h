#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace uc {

class DvdFs {
public:
    DvdFs() = default;
    DvdFs(DvdFs&& other) noexcept : e_(std::move(other.e_)), index_(std::move(other.index_)), undo_(std::move(other.undo_)), hidden_(std::move(other.hidden_)), root_(std::move(other.root_)) {}   // (the lock stays behind)
    DvdFs& operator=(DvdFs&& other) noexcept {
        e_ = std::move(other.e_); index_ = std::move(other.index_); undo_ = std::move(other.undo_); hidden_ = std::move(other.hidden_); root_ = std::move(other.root_);
        return *this;
    }
    bool open_root(const std::filesystem::path& root);
    const std::filesystem::path& disc_root() const { return root_; }   // the folder open_root was given (the game's own files, whatever the overlays hold); empty before
    // Mod files: every regular file below files_root replaces the disc file with the same path (case-insensitive; the entry keeps its
    // number) or is added with its parent folders. Symlinks and junctions are skipped, and so are folders named *.arc! (their files are members of an
    // archive, see mods/arc/arc_mods.h). Returns files replaced + added.
    // May be called while the game runs (mods are switched on and off live): the tree is locked, and entry numbers never move or change meaning.
    size_t overlay(const std::filesystem::path& files_root);
    // Takes every overlay back: replaced files return to the disc's own, added files and folders disappear (their entry numbers stay reserved and
    // are reused if the same path is added again). A game that already opened a replaced file keeps reading whatever the entry holds now.
    void clear_overlays();
    // Replaces every overlay by these (mods in load order) in one step; returns files replaced + added. Scanning happens before the lock is taken.
    size_t set_overlays(const std::vector<std::filesystem::path>& files_roots);
    int entry_of(std::string path) const;                 // -1 if absent; 0 is the root directory
    bool is_dir(int e) const { std::shared_lock lock(m_); return valid(e) && e_[size_t(e)].dir; }
    uint32_t length(int e) const { std::shared_lock lock(m_); return valid(e) ? e_[size_t(e)].size : 0; }
    int64_t read(int entry, void* dst, uint32_t len, uint32_t offset) const;   // bytes read (short at EOF), -1 on a bad entry
    // What an entry holds at this moment. A game that opened a file keeps reading this version while a mod's overlay swaps the entry (a stream that is
    // playing must not change its bytes under the player); the backing file must stay on disk for as long as the handle is open.
    struct Snapshot { int entry = -1; std::filesystem::path full; uint32_t size = 0; };
    std::optional<Snapshot> snapshot(int entry) const;   // nullopt for a bad entry or a directory
    int64_t read(const Snapshot& file, void* dst, uint32_t len, uint32_t offset) const;   // like read(entry...) on the version the snapshot names
    size_t entry_count() const { std::shared_lock lock(m_); return e_.size(); }
    std::string path_of(int e) const { std::shared_lock lock(m_); return valid(e) ? e_[size_t(e)].rel : std::string(); }
private:
    struct Entry { std::string rel; std::filesystem::path full; bool dir; uint32_t size; bool hidden = false; };
    struct Found { std::string rel; std::filesystem::path full; uint32_t size; };
    static std::vector<Found> scan(const std::filesystem::path& files_root);
    size_t apply_locked(const std::filesystem::path& files_root, const std::vector<Found>& files);
    void clear_locked();
    struct Undo { size_t slot; bool added; Entry original; };
    bool valid(int e) const { return e >= 0 && size_t(e) < e_.size() && !e_[size_t(e)].hidden; }   // caller holds the lock
    void walk(const std::filesystem::path& dir, const std::string& rel);
    size_t add_entry(Entry entry);   // a hidden slot of the same path is reused; caller holds the lock exclusively
    mutable std::shared_mutex m_;
    std::vector<Entry> e_;
    std::map<std::string, int> index_;   // lower-case relative path -> entry
    std::vector<Undo> undo_;             // what overlay() changed, in order
    std::map<std::string, size_t> hidden_;   // lower-case path -> slot of an added entry that was taken back
    std::filesystem::path root_;
};

struct DvdStats { uint64_t opens = 0, reads = 0, bytes = 0; };
DvdStats dvd_stats();
void dvd_log(bool on,bool opens=false);          // print "[dvd] open <path>" and read lines to stderr; `opens` alone prints just the opens
void dvd_hle_init(DvdFs& fs);   // defined in hle/dvd.cpp
void dvd_hle_shutdown();       // call after guests stop, while their scheduler and file tree remain alive
struct DvdSnapshot;
std::shared_ptr<const DvdSnapshot> dvd_checkpoint();
void dvd_restore(const DvdSnapshot&);

class DvdSession {
public:
    explicit DvdSession(DvdFs& fs) { dvd_hle_init(fs); }
    ~DvdSession() { dvd_hle_shutdown(); }
    DvdSession(const DvdSession&) = delete;
    DvdSession& operator=(const DvdSession&) = delete;
};

}  // namespace uc
