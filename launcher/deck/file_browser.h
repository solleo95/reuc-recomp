#pragma once
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>
namespace uc::deck {
// The Steam Deck launcher has no system file dialog in Game Mode, so it browses folders itself. This is the model (no UI): the current folder,
// its sorted entries, navigation and what can be chosen. file_browser_ui.cpp draws it.
enum class PickMode { folder, image };   // folder: the game folder (directories only); image: a disc image Wit can unpack (directories and image files)
struct BrowserEntry { std::string name; bool directory; };
bool is_image_file(const std::filesystem::path&);   // .iso .wbfs .ciso .wdf .wia (the list the Windows launcher's picker offers), any case
class FileBrowser {
public:
    static constexpr size_t npos = std::numeric_limits<size_t>::max();
    // `start` that does not exist (a card that was removed) falls back to its nearest existing parent.
    FileBrowser(std::filesystem::path start, PickMode mode, std::vector<std::filesystem::path> shortcuts);
    const std::filesystem::path& current() const { return current_; }
    const std::vector<BrowserEntry>& entries() const { return entries_; }   // folders first, then files; case-insensitive; hidden (.name) left out
    const std::vector<std::filesystem::path>& shortcuts() const { return shortcuts_; }
    const std::string& error() const { return error_; }   // why the last move failed; empty after a successful one
    PickMode mode() const { return mode_; }
    bool enter(size_t index);                        // into the folder entry; false for a file or a bad index
    bool up();                                       // to the parent; false at the top of the filesystem
    bool go(const std::filesystem::path&);           // to any folder; false (current stays, error set) when it cannot be listed
    // The path a "choose" press gives: folder mode, index == npos: the current folder; image mode: the image file at `index`. Otherwise nothing.
    std::optional<std::filesystem::path> choice(size_t index) const;
private:
    bool list(const std::filesystem::path&);
    std::filesystem::path current_;
    std::vector<BrowserEntry> entries_;
    std::vector<std::filesystem::path> shortcuts_;
    std::string error_;
    PickMode mode_;
};
// Where to jump: the last folder used (if it still exists), the home folder, every folder one or two levels below the media root
// (/run/media/<card> and /run/media/<user>/<card>).
std::vector<std::filesystem::path> default_shortcuts(const std::filesystem::path& home, const std::filesystem::path& media_root,
                                                     const std::optional<std::filesystem::path>& last_folder);
// The last folder, kept in launcher-state.json next to the launcher (key "last_folder"; other keys are preserved).
std::optional<std::filesystem::path> load_last_folder(const std::filesystem::path& state_file);
bool store_last_folder(const std::filesystem::path& state_file, const std::filesystem::path& folder);   // through a temporary file and a rename
}
