// The Steam Deck launcher's file browser model: listing, navigation, choice, shortcuts and the remembered folder.
#include "check.h"
#include "deck/file_browser.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>
using namespace uc::deck;
namespace fs = std::filesystem;
namespace {
void touch(const fs::path& path) { fs::create_directories(path.parent_path()); std::ofstream(path) << "x"; }
std::vector<std::string> names(const FileBrowser& browser) {
    std::vector<std::string> result;
    for (const auto& entry : browser.entries()) result.push_back((entry.directory ? "[" : "") + entry.name + (entry.directory ? "]" : ""));
    return result;
}
}
int main() {
    const auto root = fs::temp_directory_path() / ("uc-browser-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(root);
    fs::create_directories(root / "b_dir"); fs::create_directories(root / "A_dir"); fs::create_directories(root / ".hidden_dir");
    fs::create_directories(root / fs::u8path("\xd0\x98\xd0\xb3\xd1\x80\xd0\xb0 \xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0"));   // "Игра папка": a Cyrillic name with a space
    touch(root / "zeta.txt"); touch(root / "alpha.iso"); touch(root / "Beta.WBFS"); touch(root / ".hidden.iso"); touch(root / "c.wia"); touch(root / "d.ciso"); touch(root / "e.wdf");
    touch(root / "A_dir" / "inner.iso");

    {   // folder mode: folders only, case-insensitive order, hidden ones left out
        FileBrowser browser(root, PickMode::folder, {});
        CHECK(names(browser) == (std::vector<std::string>{"[A_dir]", "[b_dir]", "[\xd0\x98\xd0\xb3\xd1\x80\xd0\xb0 \xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0]"}));
        CHECK(browser.error().empty() && browser.current() == root);
        CHECK(browser.choice(FileBrowser::npos) == root);                       // "select this folder"
        CHECK(!browser.choice(0));                                              // a folder entry in folder mode is entered, not chosen
        CHECK(browser.enter(2) && browser.current() == root / fs::u8path("\xd0\x98\xd0\xb3\xd1\x80\xd0\xb0 \xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0"));   // non-ASCII and spaces
        CHECK(browser.up() && browser.current() == root);
    }
    {   // image mode: folders first, then only disc images, hidden ones left out
        FileBrowser browser(root, PickMode::image, {});
        CHECK(names(browser) == (std::vector<std::string>{"[A_dir]", "[b_dir]", "[\xd0\x98\xd0\xb3\xd1\x80\xd0\xb0 \xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0]", "alpha.iso", "Beta.WBFS", "c.wia", "d.ciso", "e.wdf"}));
        CHECK(browser.choice(3) == root / "alpha.iso" && browser.choice(4) == root / "Beta.WBFS");
        CHECK(!browser.choice(0) && !browser.choice(FileBrowser::npos) && !browser.choice(99));   // a folder, "this folder" and a bad index choose nothing
        CHECK(!browser.enter(3));                                               // a file cannot be entered
        CHECK(browser.enter(0) && names(browser) == (std::vector<std::string>{"inner.iso"}));
        CHECK(browser.choice(0) == root / "A_dir" / "inner.iso");
    }
    {   // going somewhere that does not work keeps the current folder and says why
        FileBrowser browser(root, PickMode::folder, {});
        CHECK(!browser.go(root / "missing") && !browser.error().empty() && browser.current() == root);
        CHECK(!browser.go(root / "zeta.txt") && browser.current() == root);        // a file
        CHECK(browser.go(root / "b_dir") && browser.error().empty());            // the error clears on success
        auto top = fs::absolute(root).root_path();
        FileBrowser at_top(top, PickMode::folder, {});
        CHECK(!at_top.up() && at_top.current() == top);                         // the top of the filesystem has no parent
        FileBrowser lost(root / "missing", PickMode::folder, {});               // a start that no longer exists falls back to an existing parent
        CHECK(lost.current() == root && lost.error().empty());
    }
    {   // shortcuts: the last folder first, the home folder, every mount one or two levels below the media root
        fs::create_directories(root / "media" / "mmcblk0p1"); fs::create_directories(root / "media" / "deck" / "USB"); fs::create_directories(root / "home");
        const auto list = default_shortcuts(root / "home", root / "media", root / "b_dir");
        CHECK(list.size() >= 4 && list[0] == root / "b_dir" && list[1] == root / "home");
        auto has = [&](const fs::path& p) { return std::find(list.begin(), list.end(), p) != list.end(); };
        CHECK(has(root / "media" / "mmcblk0p1") && has(root / "media" / "deck" / "USB"));
        CHECK(default_shortcuts(root / "home", root / "no-media", std::nullopt).size() == 1);   // no media folder, no last folder: just home
        CHECK(default_shortcuts(root / "home", root / "media", root / "gone").size() == list.size() - 1);   // a last folder that vanished is dropped
    }
    {   // the remembered folder survives a missing or damaged file
        const auto state = root / "state" / "launcher-state.json";
        CHECK(!load_last_folder(state));
        CHECK(store_last_folder(state, root / "b_dir") && load_last_folder(state) == root / "b_dir");
        CHECK(!fs::exists(fs::path(state.string() + ".new")));                  // written through a temporary and renamed
        { std::ofstream(state) << "{not json"; }
        CHECK(!load_last_folder(state));
        { std::ofstream(state) << "{\"last_folder\": 5}"; }
        CHECK(!load_last_folder(state));
        { std::ofstream(state) << "{\"other\": 1, \"last_folder\": \"/a/b\"}"; }
        CHECK(load_last_folder(state) == fs::path("/a/b"));
        CHECK(store_last_folder(state, root / "A_dir"));
        std::ifstream in(state); std::string text((std::istreambuf_iterator<char>(in)), {});
        CHECK(text.find("\"other\"") != std::string::npos);                     // keys of other writers are kept
    }
    CHECK(is_image_file("a.ISO") && is_image_file("a.wbfs") && is_image_file("x.ciso") && is_image_file("x.wdf") && is_image_file("x.wia"));
    CHECK(!is_image_file("a.txt") && !is_image_file("iso") && !is_image_file(".iso") && !is_image_file("a.iso.bak"));
    fs::remove_all(root);
    std::puts("file browser: listing, navigation, choice, shortcuts and last folder passed");
}
