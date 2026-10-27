#include "check.h"
#include "hook_registry.h"
#include "name_patch.h"
#include "ppc_rt.h"
#include <fstream>

namespace fs = std::filesystem;
using namespace uc::mods;

namespace {
constexpr uint32_t kTable = 0x803DE4C0, kAda = 0x803DE4C6, kChris = 0x803DE4E6, kRebecca = 0x803DE4FB, kJill = 0x803DE4F6, kEmpty = 0x803DE4C5, kDest = 0x80500000;

// The label table as the English main.dol has it (guest address 0x803DE4C0): keys and names back to back, each ending in a NUL.
constexpr char kTableBytes[] = "\0HG_F\0Ada\0HG_B\0Billy\0HG_C\0Carlos\0HG_A\0Chris\0HG_E\0HUNK\0Jill\0Rebecca\0Wesker\0";
const std::string kOriginal(kTableBytes, sizeof(kTableBytes) - 1);

struct Rig {
    std::vector<uint8_t> ram = std::vector<uint8_t>(0x1800000);
    GuestMemory memory{ram.data()};
    Rig() { memory.write_bytes(kTable, kOriginal.data(), uint32_t(kOriginal.size())); }

    // What the guest's strcpy(kDest, source) turns into: the hook runs first, then the function body would copy r4 to r3.
    // Returns the string at kDest after that copy (empty when the copy only moved a NUL onto itself).
    std::string copy(uint32_t source, bool& redirected) {
        static ppc::Ctx c;
        c = {};
        c.r[3] = kDest;
        c.r[4] = source;
        const std::string before = "marker";
        memory.write_bytes(kDest, before.c_str(), uint32_t(before.size() + 1));
        hook_enter<0x802933D8u>(c);
        redirected = c.r[3] == kEmpty && c.r[4] == kEmpty;
        if (!redirected) {   // the body: copy the source string
            std::string text;
            for (uint32_t a = c.r[4];; ++a) { const uint8_t b = *memory.read_u8(a); if (!b) break; text += char(b); }
            memory.write_bytes(c.r[3], text.c_str(), uint32_t(text.size() + 1));
        }
        std::string out;
        for (uint32_t a = kDest;; ++a) { const uint8_t b = *memory.read_u8(a); if (!b) break; out += char(b); }
        return out;
    }
    std::string table() const {
        std::string text(kOriginal.size(), '\0');
        memory.read_bytes(kTable, text.data(), uint32_t(text.size()));
        return text;
    }
};

fs::path fresh(const char* name, const std::string& json, const std::string& charmap = "") {
    const auto dir = fs::current_path() / name;
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir / "dol");
    std::ofstream(dir / "dol" / "names.json", std::ios::binary) << json;
    if (!charmap.empty()) std::ofstream(dir / "dol" / "charmap.json", std::ios::binary) << charmap;
    return dir;
}
}  // namespace

static void long_utf8_name_replaces_the_copy_and_undoes() {
    hook_registry().clear();
    Rig rig;
    NameUndo owner;
    // Ребекка (14 bytes, the field in the DOL holds 7) and Крис, as JSON escapes: the sources are not compiled as UTF-8
    const auto result = apply_name_patches(fresh("names_utf8", R"({"Rebecca": "Ребекка", "Chris": "Крис"})"), rig.memory, owner);
    CHECK(result.errors.empty() && result.applied == 2);
    bool redirected = false;
    CHECK(rig.copy(kRebecca, redirected) == "\xD0\xA0\xD0\xB5\xD0\xB1\xD0\xB5\xD0\xBA\xD0\xBA\xD0\xB0" && redirected);
    CHECK(rig.copy(kChris, redirected) == "\xD0\x9A\xD1\x80\xD0\xB8\xD1\x81" && redirected);
    CHECK(rig.copy(kAda, redirected) == "Ada" && !redirected);   // a name the mod does not set: the game copies the original
    CHECK(rig.copy(kTable + 1, redirected) == "HG_F" && !redirected);   // other strings are left alone
    CHECK(rig.table() == kOriginal);   // the DOL's own table is never written
    undo_name_patches(owner);
    CHECK(rig.copy(kRebecca, redirected) == "Rebecca" && !redirected);
}

static void last_mod_wins_and_the_first_comes_back() {
    hook_registry().clear();
    Rig rig;
    NameUndo first, second;
    CHECK(apply_name_patches(fresh("names_a", R"({"Jill": "A", "Ada": "A"})"), rig.memory, first).applied == 2);
    CHECK(apply_name_patches(fresh("names_b", R"({"Jill": "B"})"), rig.memory, second).applied == 1);
    bool redirected = false;
    CHECK(rig.copy(kJill, redirected) == "B" && rig.copy(kAda, redirected) == "A");
    undo_name_patches(second);
    CHECK(rig.copy(kJill, redirected) == "A");
    undo_name_patches(first);
    CHECK(rig.copy(kJill, redirected) == "Jill" && !redirected);
}

static void too_long_is_refused_and_fifteen_bytes_fit() {
    hook_registry().clear();
    Rig rig;
    NameUndo owner;
    const auto result = apply_name_patches(fresh("names_len", R"({"Ada": "123456789012345", "Jill": "1234567890123456", "Wesker": ""})"), rig.memory, owner);
    CHECK(result.applied == 1 && result.errors.size() == 2 && result.errors[0].find("'Jill'") != std::string::npos && result.errors[0].find("at most 15") != std::string::npos);
    bool redirected = false;
    CHECK(rig.copy(kAda, redirected) == "123456789012345" && redirected);
    undo_name_patches(owner);
}

static void charmap_turns_letters_into_font_codes() {
    hook_registry().clear();
    Rig rig;
    NameUndo owner;
    // Ада and Крис in a font that draws Cyrillic on Latin codes; "Q" is not in the map and stays itself
    const auto dir = fresh("names_map", R"({"Ada": "Ада", "Chris": "КрисQ"})",
                           R"({"А": "A", "д": "b", "а": "a", "К": "K", "р": "p", "и": "u", "с": 99, "bad": "x", "q": 0})");
    const auto result = apply_name_patches(dir, rig.memory, owner);
    CHECK(result.applied == 2 && result.errors.size() == 2);   // the two malformed charmap entries, nothing wrong with the names
    bool redirected = false;
    CHECK(rig.copy(kAda, redirected) == "Aba" && rig.copy(kChris, redirected) == "KpucQ");
    undo_name_patches(owner);
}

static void other_main_dol_is_left_alone() {
    hook_registry().clear();
    Rig rig;
    rig.memory.write_u8(kTable + 5, 'X');   // the byte before "Ada" is no longer a terminator
    NameUndo owner;
    const auto result = apply_name_patches(fresh("names_dol", R"({"Ada": "Aba"})"), rig.memory, owner);
    CHECK(result.applied == 0 && result.errors.size() == 1 && result.errors[0].find("another main.dol") != std::string::npos);
    bool redirected = false;
    CHECK(rig.copy(kAda, redirected) == "Ada" && !redirected);
}

static void bad_entries_and_files_are_reported() {
    hook_registry().clear();
    Rig rig;
    NameUndo owner;
    const auto result = apply_name_patches(fresh("names_bad", R"({"Nobody": "x", "Chris": 5, "HUNK": "ok", "Billy": "a\u0000b"})"), rig.memory, owner);
    CHECK(result.applied == 1 && result.errors.size() == 3);
    undo_name_patches(owner);
    const auto none = fs::current_path() / "names_none";
    fs::remove_all(none);
    fs::create_directories(none);
    CHECK(apply_name_patches(none, rig.memory, owner).errors.empty());
    CHECK(apply_name_patches(fresh("names_json", "{not json"), rig.memory, owner).errors.size() == 1);
    CHECK(apply_name_patches(fresh("names_array", "[]"), rig.memory, owner).errors.size() == 1);
}

int main() {
    long_utf8_name_replaces_the_copy_and_undoes(); last_mod_wins_and_the_first_comes_back(); too_long_is_refused_and_fifteen_bytes_fit();
    charmap_turns_letters_into_font_codes(); other_main_dol_is_left_alone(); bad_entries_and_files_are_reported();
    std::puts("ok");
}
