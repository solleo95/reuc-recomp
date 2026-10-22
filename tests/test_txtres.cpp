#include "check.h"
#include "text/txtres.h"
#include <cstring>

using namespace uc::mods;

namespace {
struct Rec { uint32_t id; std::string voice; std::vector<std::string> lines; bool share_text_with_previous = false; };

void put32(std::vector<uint8_t>& d, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) d[at + size_t(i)] = uint8_t(v >> (24 - 8 * i)); }
uint32_t get32(const std::vector<uint8_t>& d, size_t at) { return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3]; }

// Header, table, records, then a pool that begins with a leftover block (like the Russian file) and shares strings where asked.
std::vector<uint8_t> make_txtres(const std::vector<Rec>& records) {
    const uint32_t count = uint32_t(records.size());
    std::vector<uint8_t> d(0x10 + 4 * count + 20 * count, 0);
    std::memcpy(d.data(), "TRES", 4);
    put32(d, 4, 0x00010000); put32(d, 8, count); put32(d, 12, 0x10);
    const auto append = [&](const std::string& text) { const uint32_t at = uint32_t(d.size()); d.insert(d.end(), text.begin(), text.end()); d.push_back(0); return at; };
    append("leftover block");
    uint32_t previous_text = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t record = 0x10 + 4 * count + 20 * i;
        put32(d, 0x10 + 4 * i, uint32_t(record));
        put32(d, record, records[i].id);
        put32(d, record + 4, records[i].voice.empty() ? 0 : append(records[i].voice));
        put32(d, record + 8, uint32_t(records[i].lines.size()));
        uint32_t text = 0;
        if (records[i].share_text_with_previous) text = previous_text;
        else for (size_t l = 0; l < records[i].lines.size(); ++l) { const uint32_t at = append(records[i].lines[l]); if (l == 0) text = at; }
        put32(d, record + 12, text);
        previous_text = text;
    }
    return d;
}

std::vector<Rec> sample() {
    return {{1, "", {"Enter"}}, {2, "VO_0000002", {"first line", "second line"}}, {7, "", {"Share me"}}, {9, "", {"Share me"}, true}, {12, "", {"Last %s"}}};
}

size_t rec(size_t i) { return 0x10 + 4 * 5 + 20 * i; }   // offset of record i of sample()
bool read_fails(const std::vector<uint8_t>& file) { std::string error; return !read_txtres(file, error) && !error.empty(); }
}  // namespace

static void reads_records_and_voice_names() {
    std::string error;
    const auto records = read_txtres(make_txtres(sample()), error);
    CHECK(records && error.empty() && records->size() == 5);
    CHECK((*records)[0].id == 1 && (*records)[0].lines == std::vector<std::string>{"Enter"});
    CHECK((*records)[1].id == 2 && (*records)[1].lines == (std::vector<std::string>{"first line", "second line"}));
    CHECK((*records)[3].lines == (*records)[2].lines);   // a shared string reads the same for both records
    CHECK((*records)[4].lines[0] == "Last %s");
}

static void patch_without_changes_is_identical() {
    const auto file = make_txtres(sample());
    std::string error;
    const auto patch = patch_txtres(file, {}, error);
    CHECK(patch && patch->file == file && patch->unknown_ids.empty());
}

static void patch_changes_text_and_line_count() {
    const auto file = make_txtres(sample());
    std::string error;
    const auto patch = patch_txtres(file, {{1, {"a", "b", "c"}}, {2, {"only"}}}, error);
    CHECK(patch && patch->file.size() > file.size());
    const auto records = read_txtres(patch->file, error);
    CHECK(records && records->size() == 5);
    CHECK((*records)[0].lines == (std::vector<std::string>{"a", "b", "c"}) && (*records)[1].lines == std::vector<std::string>{"only"});
    CHECK((*records)[2].lines == std::vector<std::string>{"Share me"} && (*records)[4].lines == std::vector<std::string>{"Last %s"});   // neighbours untouched
    CHECK(get32(patch->file, rec(1) + 4) == get32(file, rec(1) + 4) && get32(patch->file, rec(1) + 16) == 0);   // the voice pointer and the last field are kept
    CHECK(std::equal(file.begin(), file.begin() + 0x10, patch->file.begin()));
}

static void unknown_ids_are_reported() {
    std::string error;
    const auto patch = patch_txtres(make_txtres(sample()), {{3, {"x"}}, {12, {"y"}}, {99, {"z"}}}, error);
    CHECK(patch && patch->unknown_ids == (std::vector<uint32_t>{3, 99}));
    const auto records = read_txtres(patch->file, error);
    CHECK(records && (*records)[4].lines == std::vector<std::string>{"y"});
}

static void shared_strings_are_not_disturbed() {
    const auto file = make_txtres(sample());
    std::string error;
    const auto patch = patch_txtres(file, {{7, {"changed"}}}, error);
    const auto records = read_txtres(patch->file, error);
    CHECK(records && (*records)[2].lines == std::vector<std::string>{"changed"} && (*records)[3].lines == std::vector<std::string>{"Share me"});
}

static void rejects_damaged_files() {
    const auto good = make_txtres(sample());
    CHECK(read_fails({}));
    auto bad = good; bad[0] = 'X'; CHECK(read_fails(bad));                                  // magic
    bad = good; put32(bad, 4, 0x00020000); CHECK(read_fails(bad));                          // version
    bad = good; put32(bad, 8, 100001); CHECK(read_fails(bad));                              // absurd count
    bad = good; put32(bad, 8, 5000); CHECK(read_fails(bad));                                // table past the end
    bad = good; put32(bad, 12, uint32_t(good.size())); CHECK(read_fails(bad));              // table offset past the end
    bad = good; put32(bad, rec(0) + 12, uint32_t(good.size() + 10)); CHECK(read_fails(bad));   // text pointer past the end
    bad = good; put32(bad, 0x10, uint32_t(good.size() - 4)); CHECK(read_fails(bad));        // record offset leaves no room for 20 bytes
    bad = good; bad.back() = 'x'; CHECK(read_fails(bad));                                    // the last string has no NUL
    bad = good; put32(bad, rec(4) + 8, 3); CHECK(read_fails(bad));                           // the last record claims more lines than the file holds
    bad = good; put32(bad, rec(1), 1); CHECK(read_fails(bad));                               // ids not ascending (a duplicate)
    bad = good; put32(bad, rec(0) + 4, uint32_t(good.size() + 1)); CHECK(read_fails(bad));   // voice pointer outside the file
    std::string error;
    CHECK(!patch_txtres(bad, {{1, {"x"}}}, error) && !error.empty());                        // the patcher validates too
    bad = good; bad.resize(0x10 + 4 * 5 - 2); CHECK(read_fails(bad));                        // cut inside the table
}

static void patched_file_reads_back_with_read_txtres() {
    std::string error;
    TextChanges changes;
    for (const auto& r : sample()) changes[r.id] = {"line one of " + std::to_string(r.id), "a %d b"};
    const auto patch = patch_txtres(make_txtres(sample()), changes, error);
    const auto records = read_txtres(patch->file, error);
    CHECK(records && records->size() == 5);
    for (const auto& r : *records) CHECK(r.lines == (std::vector<std::string>{"line one of " + std::to_string(r.id), "a %d b"}));
    CHECK(patch_txtres(patch->file, {{12, {"again"}}}, error));   // a patched file can be patched again
}

int main() {
    reads_records_and_voice_names();
    patch_without_changes_is_identical();
    patch_changes_text_and_line_count();
    unknown_ids_are_reported();
    shared_strings_are_not_disturbed();
    rejects_damaged_files();
    patched_file_reads_back_with_read_txtres();
    std::puts("test_txtres: ok");
    return 0;
}
