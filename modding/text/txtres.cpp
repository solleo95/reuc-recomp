#include "text/txtres.h"
#include <algorithm>
#include <cstring>

namespace uc::mods {
namespace {
constexpr uint32_t kVersion = 0x00010000, kMaxRecords = 100000, kRecordSize = 20, kMaxLinesPerRecord = 1000;

uint32_t be32(std::span<const uint8_t> d, size_t at) { return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3]; }
void put_be32(std::vector<uint8_t>& d, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) d[at + size_t(i)] = uint8_t(v >> (24 - 8 * i)); }

struct Parsed {
    std::vector<TextRecord> records;
    std::vector<uint32_t> offsets;   // file offset of each record
};

// Reads the NUL-terminated string at `at`; false when it has no terminator inside the file or the scanning budget is used up. Shared pointers are legal,
// so a hostile file could make every record scan the same megabytes: the total scanned is capped at a few times the size of the file.
bool read_string(std::span<const uint8_t> file, uint32_t at, std::string* out, uint32_t& next, uint64_t& budget) {
    if (at >= file.size()) return false;
    const void* end = std::memchr(file.data() + at, 0, std::min<uint64_t>(file.size() - at, budget));
    if (!end) return false;
    const size_t length = size_t(static_cast<const uint8_t*>(end) - (file.data() + at));
    budget -= length + 1;
    if (out) out->assign(reinterpret_cast<const char*>(file.data() + at), length);
    next = uint32_t(at + length + 1);
    return true;
}

bool parse(std::span<const uint8_t> file, Parsed& parsed, std::string& error, bool keep_lines) {
    const auto fail = [&](std::string message) { error = std::move(message); return false; };
    if (file.size() < 0x10 || std::memcmp(file.data(), "TRES", 4) != 0) return fail("not a text file (no TRES header)");
    if (be32(file, 4) != kVersion) return fail("unknown text file version");
    const uint32_t count = be32(file, 8), table = be32(file, 12);
    if (count == 0 || count > kMaxRecords) return fail("the record count is not valid");
    if (table < 0x10 || table > file.size() || uint64_t(count) * 4 > file.size() - table) return fail("the record table does not fit in the file");
    parsed.records.clear();
    parsed.offsets.clear();
    parsed.records.reserve(count);
    uint64_t budget = 4 * uint64_t(file.size()) + 65536;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t at = be32(file, table + 4 * size_t(i));
        if (at > file.size() || kRecordSize > file.size() - at) return fail("record " + std::to_string(i) + " is outside the file");
        if (at < 0x10 || (at < table + 4 * uint64_t(count) && uint64_t(at) + kRecordSize > table)) return fail("record " + std::to_string(i) + " overlaps the header or the record table");
        TextRecord record;
        record.id = be32(file, at);
        if (!parsed.records.empty() && record.id <= parsed.records.back().id) return fail("the record ids are not ascending at record " + std::to_string(i));
        const uint32_t voice = be32(file, at + 4), lines = be32(file, at + 8), text = be32(file, at + 12);
        uint32_t next = 0;
        if (voice != 0 && !read_string(file, voice, nullptr, next, budget)) return fail("the voice name of record " + std::to_string(i) + " is outside the file or too much text is shared");
        if (lines > kMaxLinesPerRecord) return fail("record " + std::to_string(i) + " claims too many lines");
        uint32_t cursor = text;
        for (uint32_t l = 0; l < lines; ++l) {
            std::string line;
            if (!read_string(file, cursor, keep_lines ? &line : nullptr, next, budget)) return fail("a line of record " + std::to_string(i) + " is outside the file, has no end, or too much text is shared");
            if (keep_lines) record.lines.push_back(std::move(line));
            cursor = next;
        }
        parsed.records.push_back(std::move(record));
        parsed.offsets.push_back(at);
    }
    std::vector<uint32_t> sorted = parsed.offsets;   // a patch rewrites two fields of a record: records may not share bytes
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); ++i) if (sorted[i] - sorted[i - 1] < kRecordSize) return fail("two records overlap");
    return true;
}
}  // namespace

std::optional<std::vector<TextRecord>> read_txtres(std::span<const uint8_t> file, std::string& error) {
    Parsed parsed;
    if (!parse(file, parsed, error, true)) return std::nullopt;
    return std::move(parsed.records);
}

std::optional<TextPatch> patch_txtres(std::span<const uint8_t> file, const TextChanges& changes, std::string& error) {
    Parsed parsed;
    if (!parse(file, parsed, error, false)) return std::nullopt;   // no lines are kept: only the ids and offsets are needed
    TextPatch patch;
    patch.file.assign(file.begin(), file.end());
    uint64_t grown = file.size();
    for (const auto& [id, lines] : changes) {
        size_t index = parsed.records.size();
        for (size_t i = 0; i < parsed.records.size(); ++i) if (parsed.records[i].id == id) { index = i; break; }
        if (index == parsed.records.size()) { patch.unknown_ids.push_back(id); continue; }
        if (lines.empty() || lines.size() > kMaxLinesPerRecord) { error = "record " + std::to_string(id) + ": the number of lines is not valid"; return std::nullopt; }
        const uint32_t text_at = uint32_t(patch.file.size());
        for (const auto& line : lines) {
            if (line.find('\0') != std::string::npos) { error = "record " + std::to_string(id) + ": a line holds a NUL character"; return std::nullopt; }
            grown += line.size() + 1;
            if (grown > 0x7FFFFFFFull) { error = "the text file would grow past 2 GB"; return std::nullopt; }
            patch.file.insert(patch.file.end(), line.begin(), line.end());
            patch.file.push_back(0);
        }
        put_be32(patch.file, parsed.offsets[index] + 8, uint32_t(lines.size()));
        put_be32(patch.file, parsed.offsets[index] + 12, text_at);
    }
    return patch;
}

}  // namespace uc::mods
