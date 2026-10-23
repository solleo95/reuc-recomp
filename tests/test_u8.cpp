#include "check.h"
#include "arc/u8.h"
#include "u8_fixture.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace uc::mods;
using namespace u8_fixture;

namespace {
// Nodes in depth-first order: 0 root, 1 a.txt, 2 dir1, 3 x.bin, 4 sub, 5 y.bin, 6 z.bin, 7 dir2, 8 w.bin.
std::vector<File> sample() {
    return {{"a.txt", bytes("alpha")}, {"dir1/x.bin", bytes("xx")}, {"dir1/sub/y.bin", bytes("yyyy")}, {"dir1/z.bin", bytes("zeta zeta zeta zeta zeta zeta zeta")}, {"dir2/w.bin", bytes("w")}};
}
size_t node(size_t i) { return 0x20 + 12 * i; }

std::optional<U8Archive> parse(const std::vector<uint8_t>& file, std::string* why = nullptr) {
    std::string error;
    auto archive = U8Archive::parse(file, error);
    if (why) *why = error;
    return archive;
}
bool has(const std::vector<uint8_t>& file, const std::vector<uint8_t>& needle) { return std::search(file.begin(), file.end(), needle.begin(), needle.end()) != file.end(); }
}  // namespace

static void parse_lists_files() {
    const auto archive = parse(make_u8(sample()));
    CHECK(archive);
    CHECK((archive->files() == std::vector<std::string>{"a.txt", "dir1/x.bin", "dir1/sub/y.bin", "dir1/z.bin", "dir2/w.bin"}));
    CHECK(archive->contains("DIR1/SUB/Y.BIN") && !archive->contains("dir1") && !archive->contains("nope") && !archive->contains("dir1/sub/nope"));
}

static void write_without_changes_is_identical() {
    for (bool reversed : {false, true}) {
        const auto file = make_u8(sample(), reversed);
        const auto archive = parse(file);
        CHECK(archive && archive->write() == file);
    }
}

static void replace_grows_and_shrinks_a_member_and_moves_the_later_ones() {
    for (bool reversed : {false, true}) {
        auto archive = parse(make_u8(sample(), reversed));
        std::string error;
        CHECK(archive->replace("dir1/x.bin", std::vector<uint8_t>(100, 0x42), error) && error.empty());   // grows
        CHECK(archive->replace("dir1/z.bin", bytes("Z"), error));                                          // shrinks
        const auto out = archive->write();
        const auto again = parse(out);
        CHECK(again && again->files() == archive->files());
        CHECK(has(out, std::vector<uint8_t>(100, 0x42)) && has(out, bytes("alpha")) && has(out, bytes("yyyy")) && !has(out, bytes("zeta zeta")));
        CHECK(again->write() == out);   // a written archive is stable
    }
}

static void replace_is_case_insensitive() {
    auto archive = parse(make_u8(sample()));
    std::string error;
    CHECK(archive->replace("DIR1/Sub/Y.bin", bytes("changed"), error));
    CHECK(has(archive->write(), bytes("changed")) && !has(archive->write(), bytes("yyyy")));
}

static void add_creates_folders_and_file() {
    auto archive = parse(make_u8(sample()));
    std::string error;
    CHECK(archive->add_or_replace("dir1/new.bin", bytes("new1"), error));              // an existing folder
    CHECK(archive->add_or_replace("fresh/deeper/new2.bin", bytes("new2"), error));    // new nested folders
    CHECK(archive->add_or_replace("dir1/sub/new3.bin", bytes("new3"), error));        // the end of a folder that is the last child of its parent
    const auto out = archive->write();
    const auto again = parse(out, &error);
    CHECK(again);
    const auto files = again->files();
    for (const char* path : {"a.txt", "dir1/x.bin", "dir1/sub/y.bin", "dir1/sub/new3.bin", "dir1/z.bin", "dir1/new.bin", "dir2/w.bin", "fresh/deeper/new2.bin"})
        CHECK(std::find(files.begin(), files.end(), path) != files.end());
    CHECK(files.size() == 8);
    CHECK(has(out, bytes("new1")) && has(out, bytes("new2")) && has(out, bytes("new3")) && has(out, bytes("alpha")));
    CHECK(again->write() == out);
}

static void add_or_replace_reports_both() {
    auto archive = parse(make_u8(sample()));
    std::string error;
    CHECK(archive->add_or_replace("a.txt", bytes("beta"), error));
    CHECK(archive->files().size() == 5 && has(archive->write(), bytes("beta")));
}

static void replace_needs_an_existing_file() {
    auto archive = parse(make_u8(sample()));
    std::string error;
    CHECK(!archive->replace("nope.bin", bytes("x"), error) && !error.empty());
    CHECK(archive->write() == make_u8(sample()));   // a failed edit changes nothing
}

static void rejects_directory_as_file_and_file_as_folder() {
    auto archive = parse(make_u8(sample()));
    std::string error;
    CHECK(!archive->replace("dir1", bytes("x"), error) && !error.empty());
    error.clear();
    CHECK(!archive->add_or_replace("dir1", bytes("x"), error) && !error.empty());
    error.clear();
    CHECK(!archive->add_or_replace("a.txt/inside.bin", bytes("x"), error) && !error.empty());
    for (const char* bad : {"", "/", "a//b", "../a", "dir1/./x.bin", "dir1\\x.bin"}) {
        error.clear();
        CHECK(!archive->add_or_replace(bad, bytes("x"), error) && !error.empty());
    }
    CHECK(archive->write() == make_u8(sample()));
}

static void rejects_damaged_archives() {
    const auto good = make_u8(sample());
    const auto damaged = [&](auto&& edit) { auto file = good; edit(file); std::string why; const bool refused = !parse(file, &why) && !why.empty(); return refused; };
    CHECK(parse(good));
    CHECK(damaged([](auto& f) { f[0] = 0; }));                                                 // magic
    CHECK(damaged([](auto& f) { put32(f, node(0) + 8, 100001); }));                            // more nodes than the limit
    CHECK(damaged([](auto& f) { put32(f, node(0) + 8, 5000); }));                              // count beyond the tables
    CHECK(damaged([](auto& f) { put32(f, node(3) + 4, uint32_t(f.size())); }));                // member offset past the end
    CHECK(damaged([](auto& f) { put32(f, node(3) + 8, 0x7FFFFFF); }));                         // member size past the end
    CHECK(damaged([](auto& f) { f[node(1) + 1] = 0xFF; f[node(1) + 2] = 0xFF; f[node(1) + 3] = 0xFF; }));   // name offset outside the pool
    CHECK(damaged([](auto& f) { put32(f, node(2) + 8, 1); }));                                 // folder ends before it starts
    CHECK(damaged([](auto& f) { put32(f, node(2) + 8, 99); }));                                // folder ends after the root
    CHECK(damaged([](auto& f) { put32(f, node(4) + 4, 7); }));                                 // wrong parent
    CHECK(damaged([](auto& f) { f[node(0)] = 0; }));                                           // the root is not a folder
    CHECK(damaged([](auto& f) { f[node(1)] = 7; }));                                           // unknown node type
    CHECK(damaged([](auto& f) { put32(f, 8, uint32_t(f.size()) + 1); }));                      // tables past the end
    CHECK(damaged([](auto& f) { put32(f, 12, 0x20); }));                                       // data area inside the tables
    CHECK(damaged([](auto& f) { f.assign(10, 0); }));                                          // truncated header
    CHECK(damaged([](auto& f) { f.clear(); }));
    CHECK(damaged([](auto& f) { f.back() = 1; f.resize(f.size() - 40); put32(f, node(8) + 8, 40); }));   // last member cut off at the end of the file
}

static void padding_and_alignment() {
    auto archive = parse(make_u8(sample()));
    std::string error;
    CHECK(archive->replace("a.txt", std::vector<uint8_t>(33, 1), error));
    CHECK(archive->add_or_replace("n/new.bin", std::vector<uint8_t>(5, 2), error));
    const auto out = archive->write();
    CHECK(out.size() % 0x20 == 0);
    const auto count = (uint32_t(out[node(0) + 8]) << 24) | (uint32_t(out[node(0) + 9]) << 16) | (uint32_t(out[node(0) + 10]) << 8) | out[node(0) + 11];
    for (uint32_t i = 1; i < count; ++i) {
        if (out[node(i)] != 0) continue;
        const uint32_t offset = (uint32_t(out[node(i) + 4]) << 24) | (uint32_t(out[node(i) + 5]) << 16) | (uint32_t(out[node(i) + 6]) << 8) | out[node(i) + 7];
        CHECK(offset % 0x20 == 0);
    }
}

static void the_whole_archive_stays_below_the_size_limit() {
    auto archive = parse(make_u8(sample()));
    std::string error;
    const std::vector<uint8_t> big(U8Archive::kMaxMemberBytes, 1);
    CHECK(archive->add_or_replace("big1.bin", big, error) && archive->add_or_replace("big2.bin", big, error) && archive->add_or_replace("big3.bin", big, error));
    const auto before = archive->files();
    CHECK(!archive->add_or_replace("big4.bin", big, error) && error.find("256 MiB") != std::string::npos);   // 4 x 64 MiB and the tables do not fit
    CHECK(!archive->replace("a.txt", big, error));                                                          // growing a member counts too
    CHECK(archive->files() == before && archive->replace("big1.bin", bytes("tiny"), error) && archive->add_or_replace("big4.bin", big, error));   // shrinking one makes room
}

static void round_trips_the_install_when_UC_GAME_DIR_is_set() {
    const char* dir = std::getenv("UC_GAME_DIR");
    if (!dir) { std::fprintf(stderr, "UC_GAME_DIR is not set: the install round trip is skipped\n"); return; }
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::path(dir) / "DATA" / "files";
    size_t same = 0, refused = 0;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file() || it->path().extension() != ".arc") continue;
        std::ifstream in(it->path(), std::ios::binary);
        const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string error;
        const auto archive = U8Archive::parse(file, error);
        if (!archive) { ++refused; CHECK(it->path().filename().string().rfind("zzz_", 0) == 0); continue; }
        CHECK(archive->write() == file);
        ++same;
    }
    std::fprintf(stderr, "install: %zu archives identical, %zu refused\n", same, refused);
    CHECK(same == 0 || same > 600);
}

int main() {
    parse_lists_files();
    write_without_changes_is_identical();
    replace_grows_and_shrinks_a_member_and_moves_the_later_ones();
    replace_is_case_insensitive();
    add_creates_folders_and_file();
    add_or_replace_reports_both();
    replace_needs_an_existing_file();
    rejects_directory_as_file_and_file_as_folder();
    rejects_damaged_archives();
    padding_and_alignment();
    the_whole_archive_stays_below_the_size_limit();
    round_trips_the_install_when_UC_GAME_DIR_is_set();
    std::puts("ok");
}
