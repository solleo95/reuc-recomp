#include "game_language.h"

#include "dvdfs.h"

#include <string>

namespace uc {
namespace {

struct Pack {
    std::string_view language;   // launcher config code
    uint32_t index;              // the guest selector target (0x80242984's switch label)
    std::string_view message;    // Data/message/mes_<message>.txtres, and the layout/movie suffix
    char font;                   // Data/sysres/font*/utf8_<font>.brfnt
    std::string_view home_button;   // Data/HomeButton2/homeBtn<suffix>.arc (table803F5388)
};

// The verified table from the DOL's selector. Japanese keeps its glyphs in font0 only and the Japanese branch of the loader (0x8003F938) opens
// just that sheet and copies it into the other slots; every other language loads all three.
constexpr Pack kPacks[] = {
    {"ja", 0, "jp", 'J', ""},
    {"en", 2, "us", 'E', "_ENG"},
    {"de", 3, "ge", 'G', "_GER"},
    {"fr", 4, "fr", 'F', "_FRA"},
    {"es", 6, "sp", 'S', "_SPA"},
    {"it", 8, "it", 'I', "_ITA"},
};

constexpr std::string_view kLayouts[] = {"option", "saveload", "select", "gameover", "result", "dialog", "game"};

bool present(const DvdFs& dvd, const std::string& path) {
    const int entry = dvd.entry_of(path);
    return entry > 0 && !dvd.is_dir(entry) && dvd.length(entry) != 0;
}

bool complete(const DvdFs& dvd, const Pack& pack) {
    const std::string message(pack.message);
    const bool japanese = message == "jp";
    if (!present(dvd, "Data/message/mes_" + message + ".txtres")) return false;
    if (japanese) {   // 0x8003F938: font0 only, copied into the other two slots by the loader
        if (!present(dvd, "Data/sysres/font0/utf8_" + std::string(1, pack.font) + ".brfnt")) return false;
    } else {
        for (int sheet = 0; sheet < 3; ++sheet) {
            if (!present(dvd, "Data/sysres/font" + std::to_string(sheet) + "/utf8_" + std::string(1, pack.font) + ".brfnt")) return false;
        }
    }
    for (const auto layout : kLayouts) {
        if (!present(dvd, "Data/layout/" + std::string(layout) + (japanese ? "" : "_" + message) + ".arc")) return false;
    }
    if (!present(dvd, japanese ? "Data/layout/title.arc" : "Data/layout/title_us.arc")) return false;
    // The localized Atract layouts, one per aspect ratio, and the language's HomeButton archive (table803F5388).
    for (const auto aspect : {"4_3", "16_9"}) {
        if (!present(dvd, "Data/layout/atract_" + std::string(aspect) + "_" + message + ".arc")) return false;
    }
    if (!present(dvd, "Data/HomeButton2/homeBtn" + std::string(pack.home_button) + ".arc")) return false;
    // The save banner: Japanese only in the JP archive (table803F9450), shared US for every other pack.
    if (!present(dvd, japanese ? "Data/sysres/savebanner_jp.arc" : "Data/sysres/savebanner_us.arc")) return false;
    const std::string subtitles = japanese || message == "us" ? std::string() : "_" + message;
    return present(dvd, "Data/message/movie_jimaku" + subtitles + ".csvtbl");
}

uint32_t g_index = 2;
constexpr GameLanguage kEnglishFallback{2, "us", 'E', false};   // English text and font; installed says the requested pack is not what runs

}  // namespace

GameLanguage select_game_language(std::string_view language, const DvdFs& dvd) {
    for (const auto& pack : kPacks) {
        if (pack.language != language) continue;
        // A partial pack is not patched up with another language's files: English stands in, whole, and says so.
        if (complete(dvd, pack)) return GameLanguage{pack.index, pack.message, pack.font, true};
        break;
    }
    return kEnglishFallback;
}

void game_language_init(uint32_t guest_index) {
    switch (guest_index) {
        case 0:
        case 2:
        case 3:
        case 4:
        case 6:
        case 8: g_index = guest_index; break;
        default: g_index = 2; break;
    }
}

uint32_t game_language_index() { return g_index; }

}  // namespace uc
