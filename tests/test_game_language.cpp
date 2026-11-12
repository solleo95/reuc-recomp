// The game's text/font language: the disc-checked launcher mapping, the English fallbacks, the startup mode rule and the replacement for the
// game's selector 0x80242984 (runtime/game_language.*, runtime/hle/game_language.cpp).
#include "check.h"
#include "dvdfs.h"
#include "game_language.h"
#include "ppc_fixture.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

void hle_GameLanguageSelect(ppc::Ctx& c);   // runtime/hle/game_language.cpp (project/hle_map.txt: @80242984 GameLanguageSelect)

namespace {

fs::path fresh(const char* name) {
    const auto dir = fs::current_path() / name;
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir);
    return dir;
}

void put(const fs::path& path, std::string_view bytes = "x") {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}

void remove_path(const fs::path& path) {
    std::error_code error;
    fs::remove_all(path, error);
}

std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// The RBUE08 disc as the checks see it: every shipped pack, with the fonts and localized layouts its tables open. Japanese keeps its font only in
// font0, like the real disc.
void full_disc(const fs::path& root) {
    for (const auto message : {"jp", "us", "ge", "fr", "sp", "it"}) put(root / "Data" / "message" / ("mes_" + std::string(message) + ".txtres"));
    for (const auto message : {"", "_ge", "_fr", "_sp", "_it"}) put(root / "Data" / "message" / ("movie_jimaku" + std::string(message) + ".csvtbl"));
    for (const auto layout : {"option", "saveload", "select", "gameover", "result", "dialog", "game"})
        for (const auto suffix : {"", "_us", "_ge", "_fr", "_sp", "_it"}) put(root / "Data" / "layout" / (std::string(layout) + suffix + ".arc"));
    put(root / "Data" / "layout" / "title.arc");
    put(root / "Data" / "layout" / "title_us.arc");
    for (const auto suffix : {"jp", "us", "ge", "fr", "sp", "it"}) {
        put(root / "Data" / "layout" / ("atract_4_3_" + std::string(suffix) + ".arc"));
        put(root / "Data" / "layout" / ("atract_16_9_" + std::string(suffix) + ".arc"));
    }
    for (const auto name : {"homeBtn.arc", "homeBtn_ENG.arc", "homeBtn_GER.arc", "homeBtn_FRA.arc", "homeBtn_SPA.arc", "homeBtn_ITA.arc"})
        put(root / "Data" / "HomeButton2" / name);
    put(root / "Data" / "sysres" / "savebanner_jp.arc");
    put(root / "Data" / "sysres" / "savebanner_us.arc");
    for (int sheet = 0; sheet < 3; ++sheet)
        for (const auto font : {"E", "G", "F", "S", "I"}) put(root / "Data" / "sysres" / ("font" + std::to_string(sheet)) / ("utf8_" + std::string(font) + ".brfnt"));
    put(root / "Data" / "sysres" / "font0" / "utf8_J.brfnt");
}

uc::DvdFs open_disc(const fs::path& root) {   // a fresh index: the checks read the tree as it is now, mutations included
    uc::DvdFs dvd;
    CHECK(dvd.open_root(root));
    return dvd;
}

void same_language(const uc::GameLanguage& language, uint32_t index, std::string_view message, char font, bool installed) {
    CHECK(language.guest_index == index);
    CHECK(language.message == message);
    CHECK(language.font == font);
    CHECK(language.installed == installed);
}

const uint32_t kVerifiedIndices[] = {0, 2, 3, 4, 6, 8};

void mapping_is_the_verified_dol_table() {
    const auto root = fresh("lang_map_disc");
    full_disc(root);
    const auto dvd = open_disc(root);
    same_language(uc::select_game_language("en", dvd), 2, "us", 'E', true);
    same_language(uc::select_game_language("de", dvd), 3, "ge", 'G', true);
    same_language(uc::select_game_language("fr", dvd), 4, "fr", 'F', true);
    same_language(uc::select_game_language("es", dvd), 6, "sp", 'S', true);   // the US console language picks index 7 (US text); the pack is 6
    same_language(uc::select_game_language("it", dvd), 8, "it", 'I', true);
    same_language(uc::select_game_language("ja", dvd), 0, "jp", 'J', true);   // Japanese keeps its glyphs in font0 only: that is enough
}

void japanese_needs_its_font0_sheet_and_unsuffixed_files() {
    const auto root = fresh("lang_jp_disc");
    full_disc(root);
    same_language(uc::select_game_language("ja", open_disc(root)), 0, "jp", 'J', true);   // J only in font0, like the disc
    const std::string files[] = {"Data/sysres/font0/utf8_J.brfnt", "Data/message/mes_jp.txtres", "Data/layout/option.arc", "Data/layout/title.arc",
                                 "Data/message/movie_jimaku.csvtbl"};
    for (const auto& file : files) {
        const auto path = root / file;
        const auto saved = read(path);
        remove_path(path);
        same_language(uc::select_game_language("ja", open_disc(root)), 2, "us", 'E', false);   // the Japanese pack is not complete
        put(path, saved);
    }
    same_language(uc::select_game_language("ja", open_disc(root)), 0, "jp", 'J', true);
}

void launcher_languages_without_a_pack_use_english() {
    const auto root = fresh("lang_unknown_disc");
    full_disc(root);
    const auto dvd = open_disc(root);   // a complete disc: the fallback is about the launcher language, not a missing file
    for (const auto language : {"ru", "pt-BR", "pl", "uk", "tr", "ko", "zh-Hans", "nl", "system", "ES", "", "es-ES"}) {
        same_language(uc::select_game_language(language, dvd), 2, "us", 'E', false);
    }
}

void missing_text_font_layout_or_subtitles_fall_back_to_english() {
    const auto root = fresh("lang_partial_disc");
    full_disc(root);
    const std::string spanish[] = {
        "Data/message/mes_sp.txtres",
        "Data/sysres/font0/utf8_S.brfnt",
        "Data/sysres/font1/utf8_S.brfnt",
        "Data/sysres/font2/utf8_S.brfnt",
        "Data/layout/option_sp.arc",
        "Data/layout/saveload_sp.arc",
        "Data/layout/select_sp.arc",
        "Data/layout/gameover_sp.arc",
        "Data/layout/result_sp.arc",
        "Data/layout/dialog_sp.arc",
        "Data/layout/game_sp.arc",
        "Data/layout/title_us.arc",
        "Data/message/movie_jimaku_sp.csvtbl",
    };
    for (const auto& file : spanish) {
        const auto path = root / file;
        const auto saved = read(path);
        remove_path(path);
        same_language(uc::select_game_language("es", open_disc(root)), 2, "us", 'E', false);   // one missing piece: English, whole
        put(path, saved);
    }
    same_language(uc::select_game_language("es", open_disc(root)), 6, "sp", 'S', true);   // the restored disc selects Spanish again
}

void empty_file_or_folder_where_a_file_is_needed_falls_back() {
    const auto root = fresh("lang_shape_disc");
    full_disc(root);
    const auto message = root / "Data" / "message" / "mes_it.txtres";
    put(message, "");
    same_language(uc::select_game_language("it", open_disc(root)), 2, "us", 'E', false);   // an empty message file is not a pack
    put(message);
    const auto font = root / "Data" / "sysres" / "font1" / "utf8_I.brfnt";
    remove_path(font);
    fs::create_directories(font);   // a folder where the font belongs
    same_language(uc::select_game_language("it", open_disc(root)), 2, "us", 'E', false);
    remove_path(font);
    put(font);
    same_language(uc::select_game_language("it", open_disc(root)), 8, "it", 'I', true);
}

// The localized Atract layouts (both aspect ratios), HomeButton archives and the save banner the language's tables open: part of a complete pack.
struct NewDependency {
    const char* language;
    uint32_t index;
    const char* message;
    char font;
    const char* files[4];
};
const NewDependency kNewDependencies[] = {
    {"ja", 0, "jp", 'J', {"Data/layout/atract_4_3_jp.arc", "Data/layout/atract_16_9_jp.arc", "Data/HomeButton2/homeBtn.arc", "Data/sysres/savebanner_jp.arc"}},
    {"en", 2, "us", 'E', {"Data/layout/atract_4_3_us.arc", "Data/layout/atract_16_9_us.arc", "Data/HomeButton2/homeBtn_ENG.arc", "Data/sysres/savebanner_us.arc"}},
    {"de", 3, "ge", 'G', {"Data/layout/atract_4_3_ge.arc", "Data/layout/atract_16_9_ge.arc", "Data/HomeButton2/homeBtn_GER.arc", "Data/sysres/savebanner_us.arc"}},
    {"fr", 4, "fr", 'F', {"Data/layout/atract_4_3_fr.arc", "Data/layout/atract_16_9_fr.arc", "Data/HomeButton2/homeBtn_FRA.arc", "Data/sysres/savebanner_us.arc"}},
    {"es", 6, "sp", 'S', {"Data/layout/atract_4_3_sp.arc", "Data/layout/atract_16_9_sp.arc", "Data/HomeButton2/homeBtn_SPA.arc", "Data/sysres/savebanner_us.arc"}},
    {"it", 8, "it", 'I', {"Data/layout/atract_4_3_it.arc", "Data/layout/atract_16_9_it.arc", "Data/HomeButton2/homeBtn_ITA.arc", "Data/sysres/savebanner_us.arc"}},
};

void localized_atract_homebutton_and_savebanner_are_needed() {
    const auto root = fresh("lang_localized_disc");
    full_disc(root);
    CHECK(root.is_absolute());   // the delete paths below are the fixture tree, never a relative surprise
    for (const auto& dependency : kNewDependencies) {
        same_language(uc::select_game_language(dependency.language, open_disc(root)), dependency.index, dependency.message, dependency.font, true);
        for (const auto file : dependency.files) {
            const auto path = root / file;
            CHECK(path.is_absolute());
            const auto saved = read(path);
            remove_path(path);   // missing: a pack is not patched up from another language's archives
            same_language(uc::select_game_language(dependency.language, open_disc(root)), 2, "us", 'E', false);
            put(path, "");       // empty: not a usable archive either
            same_language(uc::select_game_language(dependency.language, open_disc(root)), 2, "us", 'E', false);
            remove_path(path);
            fs::create_directories(path);   // a folder where the archive belongs
            same_language(uc::select_game_language(dependency.language, open_disc(root)), 2, "us", 'E', false);
            remove_path(path);
            put(path, saved);
            same_language(uc::select_game_language(dependency.language, open_disc(root)), dependency.index, dependency.message, dependency.font, true);
        }
    }
}

void a_disc_with_only_english_keeps_english() {
    const auto root = fresh("lang_english_only_disc");
    put(root / "Data/message/mes_us.txtres");
    put(root / "Data/message/movie_jimaku.csvtbl");
    for (const auto layout : {"option", "saveload", "select", "gameover", "result", "dialog", "game"}) put(root / "Data/layout" / (std::string(layout) + "_us.arc"));
    put(root / "Data/layout/title_us.arc");
    put(root / "Data/layout/atract_4_3_us.arc");
    put(root / "Data/layout/atract_16_9_us.arc");
    put(root / "Data/HomeButton2/homeBtn_ENG.arc");
    put(root / "Data/sysres/savebanner_us.arc");
    for (int sheet = 0; sheet < 3; ++sheet) put(root / "Data/sysres" / ("font" + std::to_string(sheet)) / "utf8_E.brfnt");
    const auto dvd = open_disc(root);
    same_language(uc::select_game_language("en", dvd), 2, "us", 'E', true);
    for (const auto language : {"de", "fr", "es", "it", "ja", "ru"}) same_language(uc::select_game_language(language, dvd), 2, "us", 'E', false);
}

void the_selection_reads_the_tree_it_is_given() {
    const auto root = fresh("lang_order_disc"), files = fresh("lang_order_files");
    full_disc(root);
    remove_path(root / "Data" / "message" / "mes_sp.txtres");
    auto dvd = open_disc(root);
    same_language(uc::select_game_language("es", dvd), 2, "us", 'E', false);
    // game/uc_main.cpp selects before the mods' overlays exist, so a mod cannot add a language pack the disc lacks. The checks see whatever the
    // tree holds, so the same call after an overlay would find the added file: the ordering in the startup path is what keeps mods out of it.
    put(files / "Data" / "message" / "mes_sp.txtres");
    CHECK(dvd.overlay(files) == 1);
    same_language(uc::select_game_language("es", dvd), 6, "sp", 'S', true);
    dvd.clear_overlays();
    same_language(uc::select_game_language("es", dvd), 2, "us", 'E', false);
}

void the_startup_mode_rule_forces_english_for_shared_runs() {
    CHECK(!uc::english_only_run(false, false, false, false, false));   // ordinary play uses the launcher language
    CHECK(uc::english_only_run(true, false, false, false, false));     // network play: both peers read one guest state
    CHECK(uc::english_only_run(false, true, false, false, false));     // --deterministic
    CHECK(uc::english_only_run(false, false, true, false, false));     // --det-out
    CHECK(uc::english_only_run(false, false, false, true, false));     // demo record
    CHECK(uc::english_only_run(false, false, false, false, true));     // demo play
    for (int mask = 1; mask < 32; ++mask) CHECK(uc::english_only_run(mask & 1, mask & 2, mask & 4, mask & 8, mask & 16));
}

// Guest memory the HLE check watches: the language object and the small-data area around OSGetAppGamename's name buffer.
constexpr uint32_t kObject = 0x80003000, kSda = 0x80400000, kAppId = 0x80003180;
constexpr uint32_t kAppIdOverrideSlot = kSda - 26664, kAppIdSourceSlot = kSda - 29256, kAppIdName = kSda - 26624;

std::vector<uint32_t> read_words(ppc::Ctx& c, uint32_t at, size_t count) {
    std::vector<uint32_t> words(count);
    for (size_t i = 0; i < count; ++i) words[i] = ppc::rd32(c, at + uint32_t(4 * i));
    return words;
}

struct Snapshot {
    std::vector<uint32_t> object, small_data, registers;
    uint32_t lr = 0, ctr = 0;
    void take(ppc::Ctx& c) {
        object = read_words(c, kObject, 8);
        small_data = read_words(c, kAppIdName - 32, 16);
        registers.clear();
        for (unsigned i = 0; i < 32; ++i) registers.push_back(c.r[i]);
        lr = c.lr; ctr = c.ctr;
    }
    std::vector<uint32_t> changed(ppc::Ctx& c) const {
        std::vector<uint32_t> addresses;
        const auto object_now = read_words(c, kObject, object.size()), small_data_now = read_words(c, kAppIdName - 32, small_data.size());
        for (size_t i = 0; i < object.size(); ++i) if (object[i] != object_now[i]) addresses.push_back(kObject + uint32_t(4 * i));
        for (size_t i = 0; i < small_data.size(); ++i) if (small_data[i] != small_data_now[i]) addresses.push_back(kAppIdName - 32 + uint32_t(4 * i));
        return addresses;
    }
};

void prepare_guest(ppc::Ctx& c) {
    c.r[13] = kSda;
    ppc::wr32(c, kAppIdOverrideSlot, 0);                 // the normal disc launch: OSGetAppGamename reads the header copy
    std::memcpy(c.mem + kAppId, "RBUE08", 6);            // init_lowmem's application-ID copy
    ppc::wr32(c, kAppIdName, 0xDEADBEEF);                // stale: the realistic case where only the call refreshes it
    ppc::wr8(c, kAppIdName + 4, 0xEE);
    for (uint32_t at = kObject; at < kObject + 32; at += 4) ppc::wr32(c, at, 0xA5A5A5A5);
    ppc::wr32(c, kObject + 4, 0x11111111);               // the old index the selector is about to replace
}

// The index word, then OSGetAppGamename's four application-ID bytes plus NUL over the stale buffer.
const std::vector<uint32_t> kIndexAndAppIdName{kObject + 4, kAppIdName, kAppIdName + 4};
const std::vector<uint32_t> kIndexOnly{kObject + 4};

void the_hle_stores_the_index_and_only_what_the_original_stored() {
    Fix fixture;
    auto& c = fixture.c;
    prepare_guest(c);
    c.r[3] = kObject;                                    // the language object, as the generated caller passes it
    uc::game_language_init(6);                           // Spanish: the original's Spanish branch reads OSGetAppGamename
    Snapshot before; before.take(c);
    hle_GameLanguageSelect(c);
    CHECK(ppc::rd32(c, kObject + 4) == 6);
    CHECK((before.changed(c) == kIndexAndAppIdName));    // the stored index and the app-ID name buffer, nothing else
    CHECK(ppc::rd32(c, kAppIdName) == 0x52425545 && c.mem[kAppIdName + 4] == 0);   // "RBUE\0", as OSGetAppGamename leaves it
    for (unsigned i = 0; i < 32; ++i) CHECK(c.r[i] == before.registers[i]);   // nonvolatile registers unchanged
    CHECK(c.lr == before.lr && c.ctr == before.ctr);
}

void the_hle_refreshes_the_app_id_name_on_the_english_french_and_spanish_paths_only() {
    Fix fixture;
    auto& c = fixture.c;
    prepare_guest(c);
    c.r[3] = kObject;
    uc::game_language_init(2);                           // English
    Snapshot english; english.take(c);
    hle_GameLanguageSelect(c);
    CHECK(ppc::rd32(c, kObject + 4) == 2);
    CHECK((english.changed(c) == kIndexAndAppIdName));
    CHECK(ppc::rd32(c, kAppIdName) == 0x52425545);
    for (const uint32_t index : {3u, 8u}) {              // German and Italian: the original's branch reads no application ID
        prepare_guest(c);
        c.r[3] = kObject;
        uc::game_language_init(index);
        Snapshot before; before.take(c);
        hle_GameLanguageSelect(c);
        CHECK(ppc::rd32(c, kObject + 4) == index);
        CHECK((before.changed(c) == kIndexOnly));
        CHECK(ppc::rd32(c, kAppIdName) == 0xDEADBEEF);
    }
}

void the_hle_follows_the_application_id_source_the_original_used() {
    Fix fixture;
    auto& c = fixture.c;
    prepare_guest(c);
    constexpr uint32_t alternative = 0x80005000;
    std::memcpy(c.mem + alternative, "ABCDxx", 6);
    ppc::wr32(c, kAppIdOverrideSlot, 1);                 // a non-zero slot makes OSGetAppGamename read its own buffer instead of the header
    ppc::wr32(c, kAppIdSourceSlot, alternative);
    c.r[3] = kObject;
    uc::game_language_init(4);                           // French also uses the application ID
    hle_GameLanguageSelect(c);
    CHECK(ppc::rd32(c, kObject + 4) == 4);
    CHECK(ppc::rd32(c, kAppIdName) == 0x41424344 && c.mem[kAppIdName + 4] == 0);
}

void the_initialiser_only_accepts_the_verified_indices() {
    for (const auto index : kVerifiedIndices) { uc::game_language_init(index); CHECK(uc::game_language_index() == index); }
    for (const uint32_t index : {1u, 5u, 7u, 9u, 10u, 0xFFFFFFFFu}) {   // real guest indices, but none this build ever selects
        uc::game_language_init(index);
        CHECK(uc::game_language_index() == 2);
    }
}

void the_english_fallback_never_mixes_languages() {
    const auto root = fresh("lang_mixed_disc");
    full_disc(root);
    remove_path(root / "Data" / "message" / "mes_fr.txtres");   // French text missing, French font and layouts present
    same_language(uc::select_game_language("fr", open_disc(root)), 2, "us", 'E', false);
    remove_path(root / "Data" / "message" / "mes_us.txtres");   // ... and English is damaged too: still English, marked not installed
    const auto dvd = open_disc(root);
    same_language(uc::select_game_language("fr", dvd), 2, "us", 'E', false);
    same_language(uc::select_game_language("en", dvd), 2, "us", 'E', false);
}

struct Test { const char* name; void (*run)(); };

}  // namespace

int main() {
    const Test tests[] = {
        {"mapping_is_the_verified_dol_table", mapping_is_the_verified_dol_table},
        {"japanese_needs_its_font0_sheet_and_unsuffixed_files", japanese_needs_its_font0_sheet_and_unsuffixed_files},
        {"launcher_languages_without_a_pack_use_english", launcher_languages_without_a_pack_use_english},
        {"missing_text_font_layout_or_subtitles_fall_back_to_english", missing_text_font_layout_or_subtitles_fall_back_to_english},
        {"empty_file_or_folder_where_a_file_is_needed_falls_back", empty_file_or_folder_where_a_file_is_needed_falls_back},
        {"localized_atract_homebutton_and_savebanner_are_needed", localized_atract_homebutton_and_savebanner_are_needed},
        {"a_disc_with_only_english_keeps_english", a_disc_with_only_english_keeps_english},
        {"the_selection_reads_the_tree_it_is_given", the_selection_reads_the_tree_it_is_given},
        {"the_startup_mode_rule_forces_english_for_shared_runs", the_startup_mode_rule_forces_english_for_shared_runs},
        {"the_hle_stores_the_index_and_only_what_the_original_stored", the_hle_stores_the_index_and_only_what_the_original_stored},
        {"the_hle_refreshes_the_app_id_name_on_the_english_french_and_spanish_paths_only", the_hle_refreshes_the_app_id_name_on_the_english_french_and_spanish_paths_only},
        {"the_hle_follows_the_application_id_source_the_original_used", the_hle_follows_the_application_id_source_the_original_used},
        {"the_initialiser_only_accepts_the_verified_indices", the_initialiser_only_accepts_the_verified_indices},
        {"the_english_fallback_never_mixes_languages", the_english_fallback_never_mixes_languages},
    };
    for (const auto& test : tests) {
        std::printf("%s\n", test.name);
        std::fflush(stdout);
        test.run();
    }
    std::printf("game language tests passed\n");
}
