#include "name_patch.h"
#include "hook_registry.h"
#include "mod_paths.h"
#include "ppc_rt.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <string_view>

namespace fs = std::filesystem;

namespace uc::mods {
namespace {
constexpr size_t kMaxFileBytes = 64 * 1024;
constexpr size_t kMaxNameBytes = 15;            // the slot's name field holds 16 bytes with the NUL
constexpr uint32_t kStrcpy = 0x802933D8;        // StringCopy in the hookable list
constexpr uint32_t kEmptyString = 0x803DE4C5;   // the NUL after "HG_F": strcpy(it, it) copies a NUL onto itself

// capacity: the bytes of the original name; only used to recognise the table.
struct Slot { std::string_view name; uint32_t address; uint32_t capacity; };
constexpr Slot kSlots[] = {
    {"Ada", 0x803DE4C6, 3}, {"Billy", 0x803DE4CF, 5}, {"Carlos", 0x803DE4DA, 6}, {"Chris", 0x803DE4E6, 5},
    {"HUNK", 0x803DE4F1, 4}, {"Jill", 0x803DE4F6, 4}, {"Rebecca", 0x803DE4FB, 7}, {"Wesker", 0x803DE503, 6},
};

// What the hook consults. The game thread reads it inside the strcpy hook, the menu thread changes it when a mod starts or stops.
struct Entry { const NameUndo* owner; uint32_t address; std::string text; };
std::mutex g_mutex;
std::vector<Entry> g_entries;   // load order: the last entry for an address wins
GuestMemory* g_memory = nullptr;
int g_token = 0;   // the hook's owner

void on_strcpy(ppc::Ctx& c) {
    const uint32_t source = c.r[4];
    if (source < kSlots[0].address || source > kSlots[std::size(kSlots) - 1].address) return;   // almost every call leaves here
    std::lock_guard lock(g_mutex);
    const Entry* found = nullptr;
    for (const Entry& e : g_entries) if (e.address == source) found = &e;
    if (!found || !g_memory) return;
    if (!g_memory->write_bytes(c.r[3], found->text.c_str(), uint32_t(found->text.size() + 1))) return;
    c.r[3] = c.r[4] = kEmptyString;   // the original copy now only moves a NUL onto itself
}

void sync_hook() {   // caller holds g_mutex
    static bool hooked = false;
    if (g_entries.empty() && hooked) { hook_registry().remove_owner(&g_token); hooked = false; }
    else if (!g_entries.empty() && !hooked) hooked = hook_registry().add(kStrcpy, &g_token, on_strcpy);
}

using CharMap = std::map<uint32_t, uint8_t>;   // character (code point) -> the byte the game's font draws it with

std::vector<uint32_t> code_points(const std::string& text) {
    std::vector<uint32_t> points;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t code = c;
        size_t length = 1;
        if (c >= 0xC0) {
            length = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            if (i + length > text.size()) length = text.size() - i;   // cut short: the rest is read as it is
            code = c & (0xFF >> (length + 1));
            for (size_t k = 1; k < length; ++k) code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        }
        points.push_back(code);
        i += length;
    }
    return points;
}

// <mod>/dol/charmap.json: { "К": "K", "д": "b", "ж": 126 }. The value is one character (its code point is the byte) or a whole number 1 to 255.
CharMap read_charmap(const fs::path& file, PatchResult& result) {
    CharMap map;
    std::error_code error;
    if (!fs::is_regular_file(file, error)) return map;
    const std::string label = "dol/charmap.json";
    if (fs::file_size(file, error) > kMaxFileBytes) { result.errors.push_back(label + ": file is larger than 64 KiB, skipped"); return map; }
    const auto text = read_file(file);
    if (!text) { result.errors.push_back(label + ": cannot be read"); return map; }
    const auto root = nlohmann::ordered_json::parse(*text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) { result.errors.push_back(label + ": must be a JSON object of characters to codes, skipped"); return map; }
    for (const auto& [key, value] : root.items()) {
        const auto from = code_points(key);
        long long code = -1;
        if (value.is_number_integer()) code = value.get<long long>();
        else if (value.is_string()) { const auto to = code_points(value.get<std::string>()); if (to.size() == 1) code = to[0]; }
        if (from.size() != 1 || code < 1 || code > 255) { result.errors.push_back(label + ": '" + key + "': a single character needs one character or a whole number 1 to 255 as its code"); continue; }
        map[from[0]] = static_cast<uint8_t>(code);
    }
    return map;
}

// The text as the game stores it: a mapped character is its one code byte, any other character stays its UTF-8 bytes.
std::string game_bytes(const std::string& text, const CharMap& map) {
    if (map.empty()) return text;
    std::string out;
    size_t at = 0;
    for (const uint32_t code : code_points(text)) {
        const auto mapped = map.find(code);
        const size_t length = code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
        if (mapped != map.end()) out += char(mapped->second);
        else out.append(text, at, length);
        at += length;
    }
    return out;
}
}  // namespace

PatchResult apply_name_patches(const fs::path& mod_root, GuestMemory& memory, NameUndo& owner) {
    PatchResult result;
    const fs::path file = mod_root / "dol" / "names.json";
    std::error_code error;
    if (!fs::is_regular_file(file, error)) return result;
    const std::string label = "dol/names.json";
    if (fs::file_size(file, error) > kMaxFileBytes) { result.errors.push_back(label + ": file is larger than 64 KiB, skipped"); return result; }
    const auto text = read_file(file);
    if (!text) { result.errors.push_back(label + ": cannot be read"); return result; }
    const auto root = nlohmann::ordered_json::parse(*text, nullptr, false);
    if (root.is_discarded()) { result.errors.push_back(label + ": not valid JSON"); return result; }
    if (!root.is_object()) { result.errors.push_back(label + ": must contain one JSON object of names to new text"); return result; }
    const CharMap map = read_charmap(mod_root / "dol" / "charmap.json", result);
    std::vector<Entry> entries;
    for (const auto& [key, value] : root.items()) {
        const auto fail = [&](const std::string& why) { result.errors.push_back(label + ": '" + key + "': " + why); };
        const Slot* slot = nullptr;
        for (const Slot& s : kSlots) if (s.name == key) slot = &s;
        if (!slot) { fail("not a name the game keeps here (Ada, Billy, Carlos, Chris, HUNK, Jill, Rebecca, Wesker)"); continue; }
        if (!value.is_string()) { fail("must be text"); continue; }
        const std::string bytes = game_bytes(value.get<std::string>(), map);
        if (bytes.empty() || bytes.find('\0') != std::string::npos) { fail("must be text without a NUL character"); continue; }
        if (bytes.size() > kMaxNameBytes) { fail(std::to_string(bytes.size()) + " bytes, at most " + std::to_string(kMaxNameBytes) + " fit (Cyrillic letters take 2 bytes each in UTF-8)"); continue; }
        const auto before = memory.read_u8(slot->address - 1), after = memory.read_u8(slot->address + slot->capacity), empty = memory.read_u8(kEmptyString);
        if (!before || !after || !empty || *before != 0 || *after != 0 || *empty != 0) { fail("the game's memory does not look like the name table (another main.dol?)"); continue; }
        entries.push_back({&owner, slot->address, bytes});
        ++result.applied;
    }
    if (entries.empty()) return result;
    std::lock_guard lock(g_mutex);
    g_memory = &memory;
    g_entries.insert(g_entries.end(), entries.begin(), entries.end());
    sync_hook();
    return result;
}

void undo_name_patches(NameUndo& owner) {
    std::lock_guard lock(g_mutex);
    g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(), [&](const Entry& e) { return e.owner == &owner; }), g_entries.end());
    sync_hook();
}

}  // namespace uc::mods
