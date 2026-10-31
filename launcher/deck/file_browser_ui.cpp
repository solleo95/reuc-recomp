#include "file_browser_ui.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
namespace uc::deck {
namespace fs = std::filesystem;
BrowserResult draw_file_browser(FileBrowser& browser, RowCtx& rows, const BrowserUi& ui, std::span<const DeckKey> keys, fs::path& chosen) {
    static bool shown = false;
    static std::string shown_folder;
    const float unit = rows.unit;
    const auto tr = [&](std::string_view key) { return ui.text(ui.user, key); };
    const auto display = ImGui::GetIO().DisplaySize;
    const std::string title = std::string(tr(browser.mode() == PickMode::folder ? "deck_browse_folder" : "deck_browse_image")) + "###file-browser";
    if (!shown) { ImGui::OpenPopup(title.c_str()); shown = true; shown_folder.clear(); }
    else if (!ImGui::IsPopupOpen(title.c_str())) { shown = false; return BrowserResult::cancelled; }
    ImGui::SetNextWindowSize({std::min(1000 * unit, display.x - 32 * unit), std::min(700 * unit, display.y - 32 * unit)}, ImGuiCond_Always);
    ImGui::SetNextWindowPos({display.x / 2, display.y / 2}, ImGuiCond_Always, {.5f, .5f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {22 * unit, 20 * unit});
    BrowserResult result = BrowserResult::open;
    if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav)) {
        const std::string folder = browser.current().string();
        const bool new_folder = shown_folder != folder;
        rows.focus->begin_frame(keys);
        ImGui::BeginChild("rows", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
        if (new_folder) ImGui::SetScrollY(0);
        if (button_row(rows, "back", tr("back"))) result = BrowserResult::cancelled;
        if (browser.mode() == PickMode::folder && button_row(rows, "select", tr("deck_browse_select"))) { chosen = browser.current(); result = BrowserResult::chosen; }
        if (button_row(rows, "up", tr("deck_browse_up"))) browser.up();
        for (size_t i = 0; i < browser.shortcuts().size(); ++i) {
            const auto& path = browser.shortcuts()[i];
            std::string label = path == ui.home ? std::string(tr("deck_browse_home")) : path.filename().string();
            if (label.empty()) label = path.string();
            if (button_row(rows, ("shortcut" + std::to_string(i)).c_str(), label.c_str())) browser.go(path);
        }
        note(rows, folder.c_str(), ImGui::GetColorU32(ImGuiCol_TextDisabled));
        if (!browser.error().empty()) note(rows, browser.error().c_str(), IM_COL32(217, 115, 102, 255));
        const int header_rows = int(rows.focus->count());
        if (browser.entries().empty()) note(rows, tr("deck_browse_empty"), ImGui::GetColorU32(ImGuiCol_TextDisabled));
        size_t enter_index = FileBrowser::npos;
        for (size_t i = 0; i < browser.entries().size(); ++i) {
            const auto& entry = browser.entries()[i];
            if (entry_row(rows, ("entry" + std::to_string(i)).c_str(), entry.name, entry.directory ? EntryMark::folder : EntryMark::none)) {
                if (entry.directory) enter_index = i;
                else if (auto picked = browser.choice(i)) { chosen = *picked; result = BrowserResult::chosen; }
            }
        }
        ImGui::EndChild();
        rows.focus->end_frame();
        if (enter_index != FileBrowser::npos) browser.enter(enter_index);   // after the loop: the list changes under it
        if (new_folder) { shown_folder = folder; if (!browser.entries().empty()) rows.focus->set_focus(header_rows); }   // a new folder: the focus goes to its first entry
        if (std::find(keys.begin(), keys.end(), DeckKey::back) != keys.end()) result = BrowserResult::cancelled;
        if (result != BrowserResult::open) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    if (result != BrowserResult::open) shown = false;
    return result;
}
}
