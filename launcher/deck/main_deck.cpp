#include "app.h"
#include "paths.h"
#include "graphite.h"
#include "localization.h"
#include "ui_fonts.h"
#include "update.h"
#include "update_ui.h"
#include "files_posix.h"
#include "settings_pages.h"
#include "gamepad_input.h"
#include "exec_game.h"
#include "file_browser_ui.h"
#include "game_path.h"
#include "instance_lock.h"
#include "feature_report.h"
#include "launch.h"
#include "menu_text.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <future>
#include <stdexcept>
#include <thread>
#include <unistd.h>

// The Steam Deck launcher: the look of the Windows launcher (graphite sidebar, train header), three tabs and a language select, driven by the gamepad (D-pad on the
// right part, L1/R1 on the left), by touch and by the right trackpad. Large on purpose: the Deck's screen is small.
namespace uc::deck {
namespace fs = std::filesystem;
namespace {
constexpr float kScale = 1.05f;   // the whole interface is this much larger than a 1280x800 layout at 1:1 (large on purpose: the Deck's screen is small)
struct Args { fs::path snapshot; int width = 0, height = 0; fs::path config; };
Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--snapshot" && i + 1 < argc) args.snapshot = argv[++i];
        else if (arg == "--config" && i + 1 < argc) args.config = argv[++i];
        else if (arg == "--window" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &args.width, &args.height) != 2 || args.width < 640 || args.height < 360)
                throw std::invalid_argument("--window expects WIDTHxHEIGHT, at least 640x360");
        } else throw std::invalid_argument("usage: uc_launcher [--config path] [--snapshot directory] [--window WxH]");
    }
    return args;
}
const char* tr(const App& app, std::string_view key) { return launcher::text(app.active_language, key).data(); }
const char* tr_cb(void* user, std::string_view key) { return tr(*static_cast<const App*>(user), key); }

struct Platform {   // SDL, the window, ImGui and the artwork; closes everything in reverse order
    App& app;
    uc::ui::Fonts fonts;
    bool imgui = false, platform = false, backend = false;
    explicit Platform(App& a) : app(a) {}
    ~Platform() {
        if (backend) ImGui_ImplSDLRenderer3_Shutdown();
        if (platform) ImGui_ImplSDL3_Shutdown();
        if (imgui) ImGui::DestroyContext();
        SDL_DestroyTexture(app.brand); SDL_DestroyTexture(app.banner);
        SDL_DestroyRenderer(app.renderer); SDL_DestroyWindow(app.window);
        SDL_Quit();
    }
    void init(const Args& args) {
        ::unsetenv("SDL_GAMECONTROLLER_IGNORE_DEVICES");   // Steam lists the Deck's own controller there to hide it from SDL games
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) throw std::runtime_error(SDL_GetError());
        const bool windowed = !args.snapshot.empty() || args.width;
        int width = args.width ? args.width : 1280, height = args.height ? args.height : 720;
        if (!windowed) {
            const auto* mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
            if (mode && mode->w > 0 && mode->h > 0) { width = mode->w; height = mode->h; }
            else { width = 1280; height = 800; }
        }
        app.window = SDL_CreateWindow("Resident Evil: The Umbrella Chronicles", width, height,
                                      (windowed ? 0 : SDL_WINDOW_FULLSCREEN) | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!app.window) throw std::runtime_error(SDL_GetError());
        app.renderer = SDL_CreateRenderer(app.window, nullptr);
        if (!app.renderer || !SDL_SetRenderVSync(app.renderer, 1)) throw std::runtime_error(SDL_GetError());
        ImGui::CreateContext(); imgui = true;
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);   // our own navigation drives the rows (focus.h); ImGui's only serves the update window
        launcher::graphite_style();
        ImGui::GetStyle().FontScaleDpi = 1;
        fonts = uc::ui::load_fonts(*io.Fonts); app.sans = fonts.sans; app.serif = fonts.serif;
        platform = ImGui_ImplSDL3_InitForSDLRenderer(app.window, app.renderer);
        backend = ImGui_ImplSDLRenderer3_Init(app.renderer);
        if (!platform || !backend) throw std::runtime_error("ImGui backend initialization failed");
        try {
            app.banner = launcher::load_banner(app.renderer, app.directory / "assets" / "launcher" / "graphite-train.png");
            app.brand = launcher::load_banner(app.renderer, app.directory / "assets" / "launcher" / "graphite-brand.png");
        } catch (const std::exception& e) { std::fprintf(stderr, "launcher artwork: %s\n", e.what()); }   // the artwork is decoration: the launcher works without it
        SDL_ShowWindow(app.window);
    }
};

void art(SDL_Texture* texture, ImDrawList& draw, ImVec2 at, ImVec2 size, ImVec2 source, ImVec2 end) {
    if (texture) draw.AddImage((ImTextureID)(intptr_t)texture, at, {at.x + size.x, at.y + size.y},
        {source.x / texture->w, source.y / texture->h}, {end.x / texture->w, end.y / texture->h});
}
// The header fills its area like CSS "cover": the whole picture scaled to touch both sides, the overflow cut off equally at the edges.
void cover(SDL_Texture* banner, ImDrawList& draw, ImVec2 at, ImVec2 size) {
    if (!banner || size.x <= 0 || size.y <= 0) return;
    const float width = float(banner->w), height = float(banner->h), wanted = size.x / size.y;
    if (wanted > width / height) { const float shown = width / wanted; art(banner, draw, at, size, {0, (height - shown) / 2}, {width, (height + shown) / 2}); }
    else { const float shown = height * wanted; art(banner, draw, at, size, {(width - shown) / 2, 0}, {(width + shown) / 2, height}); }
}

constexpr ImU32 kSidebarColor = IM_COL32(13, 16, 17, 255);   // the background of assets/launcher/graphite-brand.png (measured: its pixels are 13,16,17 almost everywhere)
constexpr const char* kTabLabels[kTabCount] = {"game", "graphics", "controls", "deck_page_mods"};
constexpr const char* kTabTitles[kTabCount] = {"game", "graphics_title", "controls", "deck_page_mods"};
constexpr const char* kTabHints[kTabCount] = {"game_hint", "graphics_hint", "controls_hint", "mods_hint"};
constexpr int kModsIcon = 5;   // launcher::draw_navigation_icon: the box

std::string utf8(const fs::path& path) { auto text = path.u8string(); return {text.begin(), text.end()}; }
fs::path game_location(const App& app) { return app.options.game.is_absolute() ? app.options.game : app.directory / app.options.game; }
GameState current_game_state(const App& app) { return app.pending_image.empty() ? inspect_game(game_location(app)) : GameState::image_selected; }
fs::path state_file(const App& app) { return app.directory / "launcher-state.json"; }

void open_browser(App& app, PickMode mode) {
    const auto last = load_last_folder(state_file(app));
    const char* home_env = std::getenv("HOME");
    const fs::path home = home_env ? fs::path(home_env) : app.directory;
    fs::path start = last ? *last : mode == PickMode::folder ? game_location(app).parent_path() : home;
    app.browser.emplace(start, mode, default_shortcuts(home, "/run/media", last));
}
// Starts the game: saves the settings, then the launcher is replaced by uc-game.sh. Returns only when something stopped it (the reason is in app.error).
void play(App& app) {
    if (!save_config(app, app.options)) return;
    if (app.features.valid()) app.features.wait_for(std::chrono::seconds(3));   // the GPU probe (`uc --features`) normally ends within a second: not left running beside the game
    app.error = exec_game(app.directory, {});
}
void begin_play(App& app) {
    if (!app.pending_image.empty()) {   // Play unpacks the chosen image first and goes on by itself
        app.unpack = std::make_unique<launcher::ImageUnpack>(app.directory, fs::absolute(app.pending_image));
        return;
    }
    play(app);
}

RowCtx make_rows(App& app, FocusList& focus) {
    RowCtx rows;
    rows.focus = &focus;
    rows.unit = app.unit;
    rows.serif = app.serif;
    rows.text = [&app](std::string_view key) { return std::string(tr(app, key)); };
    rows.panel_text = [&app](std::string_view key) { return tr(app, key); };
    rows.env = app.env;
    rows.open_list = [&app](const settings::Item& item) {   // A or a tap on a choice
        app.list.open = true;
        app.list.title = tr(app, item.label_key);
        app.list.labels = item.labels(app.options, app.env, [&app](std::string_view key) { return std::string(tr(app, key)); });
        app.list.current = std::max(0, item.index(app.options, app.env));
        app.list.choose = [&app, &item](int index) {
            Options chosen = app.options;
            if (settings::set_choice(item, chosen, app.env, index)) { app.options = chosen; save_config(app, chosen); }
        };
        app.modal_focus.set_focus(app.list.current);
    };
    rows.touch = &app.touch;
    rows.sub = &app.binding_sub;
    rows.hold_capture = &app.hold_capture;
    return rows;
}
void open_language_list(App& app) {
    app.list.open = true;
    app.list.title = tr(app, "language");
    app.list.labels = {tr(app, "system")};
    int current = 0, index = 1;
    for (const auto& language : launcher::languages()) { app.list.labels.emplace_back(language.name); if (app.language == language.code) current = index; ++index; }
    app.list.current = current;
    app.list.choose = [&app](int chosen) {
        app.language = chosen == 0 ? "system" : std::string(launcher::languages()[size_t(chosen - 1)].code);
        app.active_language = app.language == "system" ? app.system_language : app.language;
        save_config(app, app.options);
    };
    app.modal_focus.set_focus(current);
}

bool save_mods(App& app) {   // the Mods tab's choices go into config.json's "mods" section beside everything else
    if (!app.mods || app.blocked) return false;
    try {
        auto next = app.document;
        next.properties["mods"] = app.mods->section();
        uc::save_config(app.config_path, next);
        app.document = std::move(next); app.error.clear();
        return true;
    } catch (const std::exception& e) { app.error = e.what(); return false; }
}
bool ensure_mods(App& app) {   // read when the tab is first drawn: finding the mods unpacks their .zip files. False (with app.mods_error) when the mods folder cannot be read
    if (app.mods) return true;
    if (!app.mods_error.empty()) return false;   // it failed once: the tab says so until the config is read again
    const auto stored = app.document.properties.find("mods");
    try {
        app.mods = std::make_unique<mods::StandaloneMods>(app.mods_directory, stored != app.document.properties.end() ? *stored : nlohmann::ordered_json::object());
    } catch (const std::exception& e) { app.mods_error = e.what(); return false; }
    app.mods_ui.conflicts_known = false;
    app.mods_ui.open_list = [&app](const std::string& title, std::vector<std::string> labels, int current, std::function<void(int)> choose) {   // a mod's choice setting: the same list as the other choices
        app.list.open = true; app.list.title = title; app.list.labels = std::move(labels); app.list.current = current;
        app.list.choose = [&app, choose = std::move(choose)](int index) { choose(index); save_mods(app); };
        app.modal_focus.set_focus(current);
    };
    return true;
}
const char* mods_text(const App& app, const char* key) {   // the manager's own strings (English, Russian, else English)
    const char* text = mods::manager_text(app.active_language, key);
    return text ? text : key;
}

// The Game tab above the settings: where the game is, and the two ways to say so (a folder, a disc image).
void draw_game_panel(App& app, RowCtx& rows) {
    caption(rows, tr(app, "folder"));
    const std::string shown = app.pending_image.empty() ? utf8(game_location(app)) : utf8(app.pending_image);
    note(rows, shown.c_str(), ImGui::GetColorU32(ImGuiCol_TextDisabled));
    if (button_row(rows, "browse", tr(app, "browse"), !app.unpack)) open_browser(app, PickMode::folder);
    if (button_row(rows, "image", tr(app, "image_browse"), !app.unpack)) open_browser(app, PickMode::image);
    const auto state = current_game_state(app);
    note(rows, tr(app, state == GameState::found ? "found" : state == GameState::image_selected ? "image_selected" : "missing"),
         state == GameState::found ? IM_COL32(120, 189, 133, 255) : IM_COL32(217, 173, 107, 255));
}

// Keys of the main screen: L1/R1 move the left side's selection (tab, tab, tab, language), B asks to leave, A on the language select opens its
// list; everything else goes to the rows.
std::vector<DeckKey> handle_main_keys(App& app, const std::vector<DeckKey>& keys) {
    std::vector<DeckKey> forward;
    for (const DeckKey key : keys) {
        switch (key) {
        case DeckKey::tab_prev:
            if (app.focus.hidden()) app.focus.set_focus(app.focus.focus());   // back from the language select to the right part
            else if (int(app.tab) > 0) { app.tab = Tab(int(app.tab) - 1); app.focus.set_focus(0); app.reset_scroll = true; }
            break;
        case DeckKey::tab_next:
            if (!app.focus.hidden()) {
                if (int(app.tab) + 1 < kTabCount) { app.tab = Tab(int(app.tab) + 1); app.focus.set_focus(0); app.reset_scroll = true; }
                else app.focus.hide();   // after the last tab the language select is selected
            }
            break;
        case DeckKey::back:
            app.quit_open = true; app.modal_focus.set_focus(1);   // B on the main screen offers to leave (a capture or a modal takes B before it gets here)
            break;
        case DeckKey::accept:
            if (app.focus.hidden()) open_language_list(app); else forward.push_back(key);
            break;
        default: forward.push_back(key); break;
        }
    }
    return forward;
}

bool update_visible(const launcher::UpdateController& updates) {
    const auto state = updates.state();
    return (updates.release().has_value() || updates.error().has_value()) && state != launcher::UpdateState::disabled && state != launcher::UpdateState::checking;
}

void draw_sidebar(App& app, float sidebar, float height) {
    const float unit = app.unit;
    ImGui::BeginChild("sidebar", {sidebar, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    auto* draw = ImGui::GetWindowDrawList();
    // The logo is scaled down so that the tabs, the language select and the footer picture all fit under it.
    const float below = float(kTabCount) * 70 * unit + 100 * unit + 64 * unit + 18 * unit;
    const float logo_height = std::clamp(height - below, 200 * unit, 446 * unit);
    const float logo_scale = logo_height / (446 * unit);
    if (app.brand) art(app.brand, *draw, {std::round(sidebar * (1 - logo_scale) / 2), 0}, {std::round(sidebar * logo_scale), std::round(logo_height)}, {0, 0}, {340, 446});   // x=340..341 is a darker edge line of the picture
    else launcher::draw_umbrella(*draw, {sidebar / 2, logo_height * .45f}, 90 * unit * logo_scale);
    ImGui::SetCursorPos({8 * unit, std::round(logo_height + 6 * unit)});
    for (int i = 0; i < kTabCount; ++i) {
        ImGui::SetCursorPosX(std::round(8 * unit));
        ImGui::PushID(i);
        const auto p = ImGui::GetCursorScreenPos();
        const bool selected = int(app.tab) == i && !app.focus.hidden();   // the language select takes the highlight while it is selected
        const bool current = int(app.tab) == i;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
        ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4{.34f, .055f, .075f, 1} : current ? ImVec4{.20f, .04f, .055f, 1} : ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.30f, .07f, .09f, 1});
        if (ImGui::Button("##navigation", {sidebar - 16 * unit, 64 * unit})) { app.tab = Tab(i); app.focus.set_focus(0); app.reset_scroll = true; }
        ImGui::PopStyleColor(2); ImGui::PopStyleVar();
        const auto color = current ? IM_COL32(246, 243, 237, 255) : IM_COL32(161, 164, 164, 255);
        if (selected) draw->AddRectFilled(p, {p.x + 6 * unit, p.y + 64 * unit}, IM_COL32(200, 33, 47, 255), 2 * unit);
        launcher::draw_navigation_icon(*draw, i == int(Tab::mods) ? kModsIcon : i, {std::round(p.x + 26 * unit), std::round(p.y + 14 * unit)}, 36 * unit, color);
        draw->AddText(app.sans, std::round(27 * unit), {std::round(p.x + 80 * unit), std::round(p.y + 16 * unit)}, color, tr(app, kTabLabels[i]));
        ImGui::PopID();
    }
    // the language select: a box with the current language; selected with L1/R1 (red outline), A or a tap opens the list
    ImGui::SetCursorPos({20 * unit, ImGui::GetCursorPosY() + 12 * unit});
    ImGui::TextDisabled("%s", tr(app, "language"));
    const char* language_label = tr(app, "system");
    for (auto item : launcher::languages()) if (item.code == app.language) language_label = item.name.data();
    ImGui::SetCursorPosX(20 * unit);
    const ImVec2 at = ImGui::GetCursorScreenPos(), size{sidebar - 40 * unit, 54 * unit};
    ImGui::InvisibleButton("##language", size);
    if (ImGui::IsItemClicked()) open_language_list(app);
    const bool lit = app.focus.hidden();
    draw->AddRectFilled(at, {at.x + size.x, at.y + size.y}, lit ? IM_COL32(86, 14, 19, 230) : IM_COL32(28, 33, 34, 235), 9 * unit);
    draw->AddRect(at, {at.x + size.x, at.y + size.y}, lit ? IM_COL32(179, 35, 49, 255) : IM_COL32(70, 76, 77, 255), 9 * unit, 0, (lit ? 2.5f : 1.f) * unit);
    draw->AddText(app.sans, std::round(25 * unit), {at.x + 16 * unit, std::round(at.y + (size.y - 25 * unit) / 2)}, IM_COL32(238, 233, 220, 255), language_label);
    const float x = at.x + size.x - 26 * unit, y = at.y + size.y / 2;
    draw->AddLine({x - 8 * unit, y - 4 * unit}, {x, y + 5 * unit}, IM_COL32(238, 233, 220, 255), 2.2f * unit); draw->AddLine({x, y + 5 * unit}, {x + 8 * unit, y - 4 * unit}, IM_COL32(238, 233, 220, 255), 2.2f * unit);
    if (app.brand) {   // the footer picture (304x56 in the file), centred in the sidebar and never wider than it
        const float footer_width = std::min(304 * unit, sidebar - 24 * unit), footer_height = footer_width * 56.f / 304.f;
        art(app.brand, *draw, {std::round((sidebar - footer_width) / 2), std::round(height - footer_height - 10 * unit)}, {std::round(footer_width), std::round(footer_height)}, {0, 446}, {304, 502});
    }
    ImGui::EndChild();
}

void draw_main_screen(App& app, launcher::UpdateController& updates, const std::vector<DeckKey>& main_keys) {
    const float unit = app.unit;
    const auto display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(display);
    ImGui::Begin("Launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
    const auto available = ImGui::GetContentRegionAvail();
    const float sidebar = std::round(300 * unit);
    auto* background = ImGui::GetWindowDrawList();
    background->AddRectFilled({0, 0}, {sidebar, available.y}, kSidebarColor);   // exactly the artwork's own background, so that the pictures have no visible edge
    background->AddRectFilledMultiColor({sidebar, 0}, available, IM_COL32(25, 29, 30, 255), IM_COL32(19, 23, 24, 255), IM_COL32(15, 19, 20, 255), IM_COL32(22, 26, 27, 255));
    draw_sidebar(app, sidebar, available.y);
    ImGui::SetCursorPos({sidebar, 0});
    ImGui::BeginChild("main", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    const auto main_size = ImGui::GetContentRegionAvail();
    const auto main_origin = ImGui::GetCursorScreenPos();
    const float hero_height = std::round(96 * unit);
    if (app.banner) { cover(app.banner, *ImGui::GetWindowDrawList(), main_origin, {main_size.x, hero_height}); ImGui::Dummy({main_size.x, hero_height}); }
    else ImGui::Dummy({0, hero_height});
    auto* main_draw = ImGui::GetWindowDrawList();
    main_draw->AddLine({main_origin.x + .5f, main_origin.y}, {main_origin.x + .5f, main_origin.y + main_size.y}, IM_COL32(52, 57, 58, 255), 1);
    main_draw->AddLine({main_origin.x, main_origin.y + hero_height + .5f}, {main_origin.x + main_size.x, main_origin.y + hero_height + .5f}, IM_COL32(52, 57, 58, 255), 1);
    ImGui::SetCursorPos({32 * unit, hero_height + 14 * unit});
    ImGui::BeginChild("body", {main_size.x - 56 * unit, main_size.y - hero_height - 14 * unit}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    const int tab = int(app.tab);
    ImGui::PushFont(app.serif, std::round(38 * unit)); ImGui::TextUnformatted(tr(app, kTabTitles[tab])); ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)); ImGui::TextUnformatted(tr(app, kTabHints[tab])); ImGui::PopStyleColor();
    ImGui::Dummy({0, 4 * unit}); ImGui::Separator(); ImGui::Dummy({0, 6 * unit});

    app.focus.begin_frame(main_keys);
    RowCtx rows = make_rows(app, app.focus);
    const bool game_tab = app.tab == Tab::game;
    const bool update_blocks = updates.blocks_game_launch();
    const float play_area = game_tab ? (app.error.empty() ? 96.f : 168.f) * unit : 0.f;   // the Play button sits below the scrolling rows
    ImGui::BeginChild("settings", {0, ImGui::GetContentRegionAvail().y - play_area}, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    if (app.reset_scroll) { ImGui::SetScrollY(0); app.reset_scroll = false; }
    Options chosen = app.options;
    bool changed = false, mods_changed = false;
    if (game_tab) draw_game_panel(app, rows);
    if (app.tab == Tab::mods) {
        if (app.blocked) note(rows, app.error.c_str(), IM_COL32(217, 115, 102, 255));   // config.json could not be read: nothing is saved over it
        else if (!ensure_mods(app)) note(rows, app.mods_error.c_str(), IM_COL32(217, 115, 102, 255));   // the mods folder cannot be read; config.json is left alone
        else {
            mods_changed = draw_mods(rows, *app.mods, app.mods_ui, app.active_language);
            if (!app.error.empty()) note(rows, app.error.c_str(), IM_COL32(217, 115, 102, 255));
        }
    } else changed |= draw_tab(chosen, app.tab, rows, app.pad_editor, app.pad_family);
    ImGui::EndChild();
    if (changed && !app.blocked) { app.options = chosen; save_config(app, chosen); }
    if (mods_changed) save_mods(app);
    if (game_tab) {
        if (!app.error.empty()) note(rows, app.error.c_str(), IM_COL32(217, 115, 102, 255));
        const bool can_play = current_game_state(app) != GameState::missing && !app.unpack && !app.browser && !app.blocked && !update_blocks;
        if (button_row(rows, "play", tr(app, "play"), can_play, true)) begin_play(app);
    }
    app.focus.end_frame();
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::End();
}

// A choice's list. Drawn at the top level, or inside the window it was opened from: ImGui keeps one popup per level, so a popup opened beside another closes it.
void draw_list_modal(App& app, const std::vector<DeckKey>& keys) {
    const float unit = app.unit;
    const auto display = ImGui::GetIO().DisplaySize;
    const auto has_back = [&] { return std::find(keys.begin(), keys.end(), DeckKey::back) != keys.end(); };
    if (!ImGui::IsPopupOpen("###list")) ImGui::OpenPopup("###list");
    ImGui::SetNextWindowSize({std::min(720 * unit, display.x - 32 * unit), std::min(640 * unit, display.y - 32 * unit)}, ImGuiCond_Always);
    ImGui::SetNextWindowPos({display.x / 2, display.y / 2}, ImGuiCond_Always, {.5f, .5f});
    if (ImGui::BeginPopupModal("###list", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav)) {
        app.modal_focus.begin_frame(keys);
        RowCtx rows = make_rows(app, app.modal_focus);
        int chosen = -1;
        ImGui::PushFont(app.serif, std::round(34 * unit)); ImGui::TextUnformatted(app.list.title.c_str()); ImGui::PopFont();
        ImGui::Dummy({0, 6 * unit});
        ImGui::BeginChild("entries", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
        for (int i = 0; i < int(app.list.labels.size()); ++i)
            if (entry_row(rows, std::to_string(i).c_str(), app.list.labels[size_t(i)], i == app.list.current ? EntryMark::current : EntryMark::none)) chosen = i;
        ImGui::EndChild();
        app.modal_focus.end_frame();
        if (chosen >= 0) { auto choose = app.list.choose; app.list.open = false; ImGui::CloseCurrentPopup(); if (choose) choose(chosen); }
        else if (has_back()) { app.list.open = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

// The entry of a text setting. Drawn inside the mod's settings window (see draw_list_modal). B is left to the text field: on a keyboard it is Backspace.
void draw_text_modal(App& app, const std::vector<DeckKey>& keys) {
    const float unit = app.unit;
    const auto display = ImGui::GetIO().DisplaySize;
    auto& ui = app.mods_ui;
    const bool fresh = !ImGui::IsPopupOpen("###mod_text");
    if (fresh) { ImGui::OpenPopup("###mod_text"); app.modal_focus.set_focus(0); }
    ImGui::SetNextWindowSize({std::min(900 * unit, display.x - 32 * unit), 0}, ImGuiCond_Always);
    ImGui::SetNextWindowPos({display.x / 2, display.y / 2}, ImGuiCond_Always, {.5f, .5f});
    if (ImGui::BeginPopupModal("###mod_text", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav)) {
        app.modal_focus.begin_frame(keys);
        RowCtx rows = make_rows(app, app.modal_focus);
        ImGui::PushFont(app.serif, std::round(34 * unit)); ImGui::TextUnformatted(ui.text_key.c_str()); ImGui::PopFont();
        ImGui::Dummy({0, 4 * unit});
        if (fresh) ImGui::SetKeyboardFocusHere();   // typing starts at once (and asks the system for its keyboard)
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##text", &ui.text_buffer);
        ImGui::Dummy({0, 8 * unit});
        const bool done = button_row(rows, "done", tr(app, "close"));   // keeps the text
        const bool cancel = button_row(rows, "cancel", tr(app, "back"));
        app.modal_focus.end_frame();
        if (done) {   // a value the mod refuses (over 1024 bytes) leaves the old one
            if (auto* settings = app.mods->settings(ui.settings_id)) if (settings->set(ui.text_key, ui.text_buffer)) save_mods(app);
        }
        if (done || cancel) { ui.text_open = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

bool mods_modal_open(const App& app) {
    return !app.mods_ui.settings_id.empty() || app.mods_ui.native_warning || !app.mods_ui.confirm_id.empty() || app.mods_ui.text_open;
}

// The Mods tab's windows: a mod's settings (rows; its choice list and text entry open over it), the warning before native code is allowed, the DLL confirmation and the
// text entry of a text setting. Like the other modals they take every key while open; the settings window keeps its own focus under the windows opened over it.
void draw_mods_modals(App& app, const std::vector<DeckKey>& keys) {
    if (!app.mods) return;
    const float unit = app.unit;
    const auto display = ImGui::GetIO().DisplaySize;
    const auto& language = app.active_language;
    const auto has_back = [&] { return std::find(keys.begin(), keys.end(), DeckKey::back) != keys.end(); };
    auto& ui = app.mods_ui;
    auto& registry = app.mods->registry();
    const auto centre = [&](float width) {
        ImGui::SetNextWindowSize({std::min(width * unit, display.x - 32 * unit), 0}, ImGuiCond_Always);
        ImGui::SetNextWindowPos({display.x / 2, display.y / 2}, ImGuiCond_Always, {.5f, .5f});
    };
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav;
    // Which layer takes this frame's keys is decided once, from what was open when the frame started: a window opened during the frame gets its keys from the next one,
    // and the key that opened it is not also given to the window below.
    const bool list_was_open = app.list.open, text_was_open = ui.text_open;
    if (!ui.settings_id.empty()) {
        if (!ImGui::IsPopupOpen("###mod_settings")) { ImGui::OpenPopup("###mod_settings"); app.settings_focus.set_focus(0); }
        ImGui::SetNextWindowSize({std::min(900 * unit, display.x - 32 * unit), std::min(640 * unit, display.y - 32 * unit)}, ImGuiCond_Always);
        ImGui::SetNextWindowPos({display.x / 2, display.y / 2}, ImGuiCond_Always, {.5f, .5f});
        if (ImGui::BeginPopupModal("###mod_settings", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav)) {
            const bool top = !list_was_open && !text_was_open;   // a list or the text entry over this window has the keys
            app.settings_focus.begin_frame(top ? keys : std::vector<DeckKey>{});
            RowCtx rows = make_rows(app, app.settings_focus);
            std::string title = ui.settings_id;
            for (const auto& entry : registry.entries())
                if (entry.manifest.id == ui.settings_id) { const auto at = entry.manifest.names.find(language); title = at != entry.manifest.names.end() ? at->second : entry.manifest.name; }
            ImGui::PushFont(app.serif, std::round(34 * unit)); ImGui::TextUnformatted(title.c_str()); ImGui::PopFont();
            ImGui::Dummy({0, 6 * unit});
            ImGui::BeginChild("mod-settings-rows", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
            const bool changed = draw_mod_settings(rows, *app.mods, ui, language);
            ImGui::EndChild();
            app.settings_focus.end_frame();
            if (changed) save_mods(app);
            if (app.list.open) draw_list_modal(app, list_was_open ? keys : std::vector<DeckKey>{});   // inside this window: a popup beside it would close it
            if (ui.text_open) draw_text_modal(app, text_was_open ? keys : std::vector<DeckKey>{});
            if (ui.settings_id.empty() || (top && has_back())) { ui.settings_id.clear(); ui.text_open = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
    if (ui.native_warning) {
        if (!ImGui::IsPopupOpen("###native_warning")) { ImGui::OpenPopup("###native_warning"); app.modal_focus.set_focus(1); }
        centre(760);
        if (ImGui::BeginPopupModal("###native_warning", nullptr, kFlags)) {
            app.modal_focus.begin_frame(keys);
            RowCtx rows = make_rows(app, app.modal_focus);
            ImGui::PushFont(app.serif, std::round(34 * unit)); ImGui::TextUnformatted(mods_text(app, "native_title")); ImGui::PopFont();
            ImGui::Dummy({0, 4 * unit});
            note(rows, mods_text(app, "native_warning"), ImGui::GetColorU32(ImGuiCol_Text));
            const bool allow = button_row(rows, "allow", mods_text(app, "ok"));
            const bool cancel = button_row(rows, "cancel", mods_text(app, "cancel"));
            app.modal_focus.end_frame();
            if (allow) { registry.set_allow_native(true); save_mods(app); ui.conflicts_known = false; }
            if (allow || cancel || has_back()) { ui.native_warning = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
    if (!ui.confirm_id.empty()) {
        if (!ImGui::IsPopupOpen("###native_confirm")) { ImGui::OpenPopup("###native_confirm"); app.modal_focus.set_focus(1); }
        centre(900);
        if (ImGui::BeginPopupModal("###native_confirm", nullptr, kFlags)) {
            app.modal_focus.begin_frame(keys);
            RowCtx rows = make_rows(app, app.modal_focus);
            const std::string hash = app.mods->dll_hash(ui.confirm_id);
            ImGui::PushFont(app.serif, std::round(34 * unit)); ImGui::TextUnformatted(mods_text(app, "confirm_title")); ImGui::PopFont();
            ImGui::Dummy({0, 4 * unit});
            note(rows, mods_text(app, "confirm_text"), ImGui::GetColorU32(ImGuiCol_Text));
            note(rows, hash.c_str(), ImGui::GetColorU32(ImGuiCol_TextDisabled));
            const bool confirmed = button_row(rows, "confirm", mods_text(app, "ok"), !hash.empty());
            const bool cancel = button_row(rows, "cancel", mods_text(app, "cancel"));
            app.modal_focus.end_frame();
            if (confirmed) { registry.set_native_hash(ui.confirm_id, hash); save_mods(app); }
            if (confirmed || cancel || has_back()) { ui.confirm_id.clear(); ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
}

// The modal windows over the launcher: a choice's list, the question about leaving, the file browser and the unpacking of a disc image. They take every key while open.
void draw_modals(App& app, bool update_blocks, const std::vector<DeckKey>& keys) {
    const float unit = app.unit;
    const auto display = ImGui::GetIO().DisplaySize;
    const auto has_back = [&] { return std::find(keys.begin(), keys.end(), DeckKey::back) != keys.end(); };
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24 * unit, 22 * unit});
    if (app.list.open && app.mods_ui.settings_id.empty()) draw_list_modal(app, keys);   // over a mod's settings window the list is drawn inside it (draw_mods_modals)
    if (app.quit_open) {
        if (!ImGui::IsPopupOpen("###quit")) ImGui::OpenPopup("###quit");
        ImGui::SetNextWindowSize({std::min(760 * unit, display.x - 32 * unit), 0}, ImGuiCond_Always);
        ImGui::SetNextWindowPos({display.x / 2, display.y / 2}, ImGuiCond_Always, {.5f, .5f});
        if (ImGui::BeginPopupModal("###quit", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav)) {
            app.modal_focus.begin_frame(keys);
            RowCtx rows = make_rows(app, app.modal_focus);
            ImGui::PushFont(app.serif, std::round(34 * unit)); ImGui::TextUnformatted(tr(app, "deck_quit_title")); ImGui::PopFont();
            ImGui::Dummy({0, 4 * unit});
            note(rows, tr(app, "deck_quit_text"), ImGui::GetColorU32(ImGuiCol_Text));
            const bool close = button_row(rows, "close", tr(app, "close"));
            const bool stay = button_row(rows, "stay", tr(app, "back"));
            app.modal_focus.end_frame();
            if (close) { app.quit = true; app.quit_open = false; ImGui::CloseCurrentPopup(); }
            else if (stay || has_back()) { app.quit_open = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
    if (app.browser) {
        BrowserUi ui;
        ui.text = tr_cb; ui.user = &app;
        const char* home = std::getenv("HOME");
        ui.home = home ? fs::path(home) : fs::path();
        RowCtx rows = make_rows(app, app.modal_focus);
        fs::path chosen;
        const auto mode = app.browser->mode();
        const auto result = draw_file_browser(*app.browser, rows, ui, keys, chosen);
        if (result == BrowserResult::chosen) {
            store_last_folder(state_file(app), mode == PickMode::folder ? chosen : chosen.parent_path());
            if (mode == PickMode::folder) { app.pending_image.clear(); app.options.game = chosen; save_config(app, app.options); }
            else app.pending_image = chosen;   // not saved: the folder configured before stays in config.json until the unpack worked
        }
        if (result != BrowserResult::open) app.browser.reset();
    }
    if (app.unpack) {
        if (!ImGui::IsPopupOpen("###unpack")) ImGui::OpenPopup("###unpack");
        ImGui::SetNextWindowSize({std::min(760 * unit, display.x - 32 * unit), 0}, ImGuiCond_Always);
        ImGui::SetNextWindowPos({display.x / 2, display.y / 2}, ImGuiCond_Always, {.5f, .5f});
        if (ImGui::BeginPopupModal("###unpack", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav)) {
            app.modal_focus.begin_frame(keys);
            RowCtx rows = make_rows(app, app.modal_focus);
            ImGui::PushFont(app.serif, std::round(34 * unit)); ImGui::TextUnformatted(tr(app, "unpack_title")); ImGui::PopFont();
            ImGui::Dummy({0, 4 * unit});
            const auto state = app.unpack->status();
            if (state == launcher::UnpackStatus::running) {
                note(rows, tr(app, "unpack_working"), ImGui::GetColorU32(ImGuiCol_Text));
                ImGui::ProgressBar(app.unpack->percent() / 100.f, {-1, 26 * unit});
                ImGui::Dummy({0, 8 * unit});
                if (button_row(rows, "cancel", tr(app, "update_cancel")) || has_back()) app.unpack->cancel();
            } else if (state == launcher::UnpackStatus::done) {
                app.options.game = app.directory / "game"; app.pending_image.clear();
                app.unpack.reset(); ImGui::CloseCurrentPopup();
                if (update_blocks) save_config(app, app.options);   // an update was accepted while the image unpacked: the game is not started under it, Play does it afterwards
                else play(app);
            } else {
                if (state == launcher::UnpackStatus::failed) {
                    using launcher::UnpackError;
                    const auto reason = app.unpack->error();
                    note(rows, tr(app, reason == UnpackError::wit_missing ? "unpack_wit_missing" : reason == UnpackError::wrong_image ? "unpack_wrong_image"
                        : reason == UnpackError::image_in_game_folder ? "unpack_inside_game" : reason == UnpackError::no_space ? "unpack_no_space" : "unpack_failed"), ImGui::GetColorU32(ImGuiCol_Text));
                }
                if (button_row(rows, "close", tr(app, "unpack_close")) || has_back() || state == launcher::UnpackStatus::cancelled) { app.unpack.reset(); ImGui::CloseCurrentPopup(); }
            }
            app.modal_focus.end_frame();
            ImGui::EndPopup();
        }
    }
    draw_mods_modals(app, keys);
    ImGui::PopStyleVar();
}

void render(App& app) {
    ImGui::Render();
    SDL_SetRenderDrawColor(app.renderer, 18, 19, 22, 255);
    SDL_RenderClear(app.renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), app.renderer);
}
// One frame of the whole screen. `keys` are this frame's DeckKeys; they go to the topmost layer: the update window (ImGui's own pad navigation, switched on only while it is
// open), a modal, or the main screen.
void frame(App& app, launcher::UpdateController& updates, uint64_t now_ns) {
    auto& io = ImGui::GetIO();
    ImGui_ImplSDLRenderer3_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();
    int width = 0, height = 0;
    SDL_GetWindowSize(app.window, &width, &height);
    app.unit = kScale * std::max(.45f, std::min(width / 1280.f, height / 800.f));
    launcher::graphite_style();
    ImGui::GetStyle().ScaleAllSizes(app.unit);
    ImGui::GetStyle().FrameBorderSize = ImGui::GetStyle().PopupBorderSize = 1;   // ScaleAllSizes floors thin borders to zero below 1x
    ImGui::GetStyle().MouseCursorScale = 1;

    std::vector<DeckKey> keys = app.keys.take(now_ns);
    if (app.pad_editor.capturing() || app.hold_capture) {   // every pad button belongs to the binding (or the gyro's hold button) that waits for one
        keys.clear();
        if (!app.capture_since_ns) app.capture_since_ns = now_ns;
        if (now_ns - app.capture_since_ns > 8000000000ull) { app.pad_editor.cancel_capture(); app.hold_capture = false; }   // nobody pressed a button: the wait ends
    } else app.capture_since_ns = 0;
    const bool update_open = update_visible(updates);
    if (update_open) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad; else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    const bool modal = app.list.open || app.quit_open || app.browser || app.unpack || mods_modal_open(app);
    std::vector<DeckKey> main_keys, modal_keys;
    if (update_open) {}
    else if (modal) modal_keys = keys;
    else main_keys = handle_main_keys(app, keys);

    ImGui::PushFont(app.sans, std::round(22 * app.unit));
    draw_main_screen(app, updates, main_keys);
    draw_modals(app, updates.blocks_game_launch(), modal_keys);
    launcher::draw_update_modal(updates, app.active_language, false, app.unit);
    ImGui::PopFont();
}
void capture(SDL_Renderer* renderer, const fs::path& path) {
    SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
    if (!surface) throw std::runtime_error(SDL_GetError());
    const bool saved = SDL_SaveBMP(surface, path.c_str());
    SDL_DestroySurface(surface);
    if (!saved) throw std::runtime_error(SDL_GetError());
}
}

void reload_config(App& app) {
    try {
        auto loaded = load_config(app.config_path);
        auto settings = options_from_config(loaded);
        const auto locale = launcher::config_language(loaded);
        app.document = std::move(loaded); app.options = app.saved_options = settings;
        app.pad_editor = BindingEditor(settings.gamepad.bindings, BindingTable::gamepad);
        app.language = app.saved_language = locale;
        app.mods.reset(); app.mods_error.clear();   // the file was read again: the Mods tab reads its mods again when it is next drawn
        app.blocked = false; app.error.clear();
    } catch (const std::exception& e) { app.error = e.what(); app.blocked = true; }
}
bool save_config(App& app, const Options& chosen) {
    try {
        auto next = app.document;
        store_options(next, chosen);
        launcher::store_language(next, app.language);
        uc::save_config(app.config_path, next);
        app.document = std::move(next); app.saved_options = chosen; app.saved_language = app.language; app.error.clear();
        return true;
    } catch (const std::exception& e) { app.error = e.what(); return false; }
}

int run(int argc, char** argv) {
    const auto args = parse_args(argc, argv);
    App app;
    app.directory = executable_dir();
    if (::chdir(app.directory.c_str()) != 0) std::fprintf(stderr, "cannot enter %s\n", app.directory.c_str());
    ensure_executables(app.directory);   // a zip extracted on the Deck by a tool that dropped the Unix modes
    app.config_path = args.config.empty() ? app.directory / "config.json" : args.config;
    app.mods_directory = app.directory / "mods";
    const bool snapshot = !args.snapshot.empty();
    if (snapshot && args.config.empty()) app.config_path = args.snapshot / "config.json";   // the scripted keys change settings: never the install's own config.json
    if (snapshot) app.mods_directory = args.snapshot / "mods";                              // and never the install's own mods
    const auto instance = snapshot ? InstanceLock::acquire("/nonexistent") : InstanceLock::acquire(app.directory);   // held for the whole run; a snapshot run may sit beside a real launcher
    if (instance.state() == InstanceLock::State::busy) {   // another launcher of this install is open: its recovery and updates must not meet ours
        std::fprintf(stderr, "uc_launcher: another launcher is already running in %s\n", app.directory.c_str());
        return 0;
    }
    const auto recovery = snapshot ? launcher::PendingRecovery{} : launcher::recover_pending_update(app.directory);   // before anything reads or writes the install
    const auto& confirm = recovery.confirm;
    app.system_language = launcher::system_language(); app.active_language = app.system_language;
    reload_config(app);
    app.active_language = app.language == "system" ? app.system_language : app.language;
    if (!recovery.problem.empty()) app.error = std::string(tr(app, "error")) + ": " + recovery.problem;   // a half-updated install is announced on the Game tab
    Platform platform(app);
    platform.init(args);
    if (const auto* mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay()); mode && mode->w > 0 && mode->h > 0) {
        app.env.monitor_width = uint32_t(mode->w); app.env.monitor_height = uint32_t(mode->h);
    }
    app.env.vendor_upscalers = false;   // no DLSS or FSR on the Deck
    std::optional<GamepadInput> pads;   // after the platform: the pads close before SDL quits; the launcher works without them
    try {
        pads.emplace();
        pads->capture_sink([&app](PhysicalInput input, bool down) {   // a pad press for a binding, or for the gyro's hold button, that waits for one
            app.pad_editor.event(input, down);
            if (app.hold_capture && down && gyro_hold_allowed(input)) {   // another button is ignored: the row keeps waiting
                app.options.gamepad.gyro_hold = input; app.hold_capture = false; save_config(app, app.options);
            }
        });
    } catch (const std::exception&) { pads.reset(); }
    if (!snapshot && fs::is_regular_file(app.directory / "uc")) {   // what the GPU supports, asked of uc itself (it must not hold up the window, and a hung driver must not hold up quitting)
        auto promise = std::make_shared<std::promise<std::string>>();
        app.features = promise->get_future();
        std::thread([promise, exe = app.directory / "uc", config = app.config_path, backend = app.options.renderer_backend] {
            try { promise->set_value(query_features(exe, config, 30000, backend)); } catch (...) { promise->set_exception(std::current_exception()); }
        }).detach();
    }
    launcher::UpdateController updates(app.directory);
    std::optional<update::Manifest> installed;
    if (!snapshot && fs::exists(app.directory / "release.json")) {
        auto raw = update::read_text(app.directory / "release.json", update::max_manifest_size);
        if (!update::failed(raw)) {
            auto manifest = update::parse_manifest(std::get<std::string>(raw));
            if (!update::failed(manifest)) installed = std::get<update::Manifest>(std::move(manifest));
            else std::fprintf(stderr, "[update] Release metadata invalid; automatic update disabled\n");
        } else std::fprintf(stderr, "[update] Release metadata unavailable; automatic update disabled\n");
    }
    updates.start(std::move(installed));
    if (snapshot) {   // the screens the owner cares about, reached with the same keys a pad sends: each shot is a few frames after its keys
        fs::create_directories(args.snapshot);
        const auto shot = [&](const char* name, std::vector<DeckKey> keys) {
            for (const DeckKey key : keys) app.keys.press(key);
            for (int i = 0; i < 6; ++i) {
                SDL_Event event; while (SDL_PollEvent(&event)) ImGui_ImplSDL3_ProcessEvent(&event);
                frame(app, updates, SDL_GetTicksNS()); render(app);
                if (i == 5 && name) capture(app.renderer, args.snapshot / (std::string(name) + ".bmp"));
                SDL_RenderPresent(app.renderer);
            }
        };
        using K = DeckKey;
        shot("game", {});
        shot("game-down", {K::down, K::down});
        shot("game-play", std::vector<DeckKey>(40, K::down));   // the last row, the Play button (the script stages a game folder so that it is enabled)
        shot("graphics", {K::tab_next});
        app.language = app.active_language = "ru";   // long texts: what does not fit beside its label goes under it
        shot("graphics-ru", {});
        app.language = "system"; app.active_language = app.system_language;
        shot("graphics-down", {K::down, K::down, K::down});
        shot("controls", {K::tab_next});
        shot(nullptr, {K::accept});               // the gyro's list opens on the first row...
        shot("choice-list", {});
        shot(nullptr, {K::down});                 // ...Hold is one down from Always
        shot(nullptr, {K::accept});
        shot("gyro-hold", {});                    // "while a button is held": a row for the button appears under the mode
        shot(nullptr, {K::down, K::accept});      // A on it waits for a press
        shot("gyro-hold-waiting", {});
        app.hold_capture = false;                 // (no pad here: the wait is ended by hand)
        shot(nullptr, {K::up, K::accept});        // back to the mode row; its list opens on Hold
        shot(nullptr, {K::up, K::up});            // Off is two up
        shot(nullptr, {K::accept});
        shot("controls-gyro-off", {});            // the gyro's sensitivity, inversion and button rows are gone
        shot("slider-right", {K::down, K::right, K::right, K::right});   // Aim speed: the D-pad changes a slider at once, no A first (120% -> 150%)
        shot("controls-down", {K::down, K::down, K::down, K::down});
        shot("bindings", {K::down, K::down, K::down, K::down});
        shot("bindings-plus", {K::right});        // the D-pad reaches the plus...
        shot("bindings-clear", {K::right});       // ...and the cross of the focused action
        shot("quit-on-row", {K::back});           // B on any row asks about leaving
        shot(nullptr, {K::back});                 // B closes it
        shot("mods", {K::tab_next});              // the Mods tab (the mods of <snapshot folder>/mods)
        shot("mods-gear", {K::down, K::right});   // the first mod's gear is selected...
        shot("mod-settings", {K::accept});        // ...A opens the mod's settings
        shot(nullptr, {K::back});                 // B closes them
        shot("mod-settings-demo", {K::down, K::down, K::down, K::down, K::right, K::accept});   // the last mod (a demo with a choice, a text and a number): its settings
        shot("mod-choice-list", {K::accept});     // A on the choice opens its list over the settings window (which stays open under it)...
        shot("mod-choice-picked", {K::down, K::accept});   // ...A picks the second entry: the list closes, the settings window remains with the new value
        shot("mod-text-entry", {K::down, K::accept});      // A on the text opens its entry
        shot("mod-text-done", {K::accept});       // A on Done keeps it; the settings window remains
        shot(nullptr, {K::down, K::down, K::accept});      // Close
        shot("mod-enabled", {K::up, K::up, K::up, K::up, K::accept});   // back to the first mod: A switches it on (there is no native-code switch on Linux)
        shot("language", {K::tab_next});
        shot("language-list", {K::accept});
        shot(nullptr, {K::back});                 // B closes the list; B again asks about leaving
        shot("quit", {K::back});
        shot(nullptr, {K::back});
        shot(nullptr, {K::tab_prev, K::tab_prev, K::tab_prev});
        open_browser(app, PickMode::image);
        shot("browser", {});
        return 0;
    }
    bool confirmed = !confirm;
    while (!app.quit) {
        updates.poll();
        if (updates.close_requested()) { updates.handoff(); launcher::restart_launcher(app.directory / "uc_launcher", updates.applied_operation().filename().string(), argv); break; }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) app.quit = true;
            if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) app.pad_editor.focus(false);
            if (pads) pads->event(event, nullptr);
            if (app.pad_editor.capturing() || app.hold_capture) {   // a pad press belongs to the binding being captured, not to the menu; Escape on a keyboard gives up
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE) { app.pad_editor.cancel_capture(); app.hold_capture = false; }
            } else app.keys.event(event, SDL_GetTicksNS());
            ImGui_ImplSDL3_ProcessEvent(&event);
        }
        if (pads) app.pad_family = pads->status().family;
        if (app.features.valid() && app.features.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                const auto report = feature_report_from_json(app.features.get());
                app.env.features_known = true; app.env.effects = report.effects; app.env.anisotropy_max = report.anisotropy_max;
            } catch (const std::exception& e) { std::fprintf(stderr, "GPU report failed: %s\n", e.what()); }
        }
        frame(app, updates, SDL_GetTicksNS()); render(app);
        SDL_RenderPresent(app.renderer);
        if (!confirmed) {   // the first frame of an updated launcher is on screen: the update is good
            auto status = update::finish_install(*confirm);
            if (update::failed(status)) std::fprintf(stderr, "[update] Cannot commit the update: %s\n", std::get<update::Error>(status).detail.c_str());
            confirmed = true;
        }
    }
    if (app.language != app.saved_language) save_config(app, app.options);
    return 0;
}
}

int main(int argc, char** argv) {
    try { return uc::deck::run(argc, argv); }
    catch (const std::exception& e) { std::fprintf(stderr, "uc_launcher: %s\n", e.what()); return 1; }
}
