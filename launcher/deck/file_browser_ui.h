#pragma once
#include "file_browser.h"
#include "focus.h"
#include "rows.h"
#include <filesystem>
#include <span>
#include <string_view>
namespace uc::deck {
enum class BrowserResult { open, chosen, cancelled };
struct BrowserUi {
    const char* (*text)(void* user, std::string_view key) = nullptr;   // a translated launcher string
    void* user = nullptr;
    std::filesystem::path home;                                         // the label of its shortcut is "Home folder"
};
// The browser as a modal window over the launcher. Its controls are rows like the rest of the launcher: Back, "Select this folder" (folder mode), Up, the shortcuts, then
// the entries; the D-pad moves, A opens a folder or chooses, B goes back. Taps and finger drags do the same. Call every frame while it is open with the keys of the frame;
// `chosen` receives the path on BrowserResult::chosen.
BrowserResult draw_file_browser(FileBrowser&, RowCtx&, const BrowserUi&, std::span<const DeckKey> keys, std::filesystem::path& chosen);
}
