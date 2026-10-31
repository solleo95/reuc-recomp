// Linux/Deck only (tests/linux/CMakeLists.txt). Archives are written here as stored ZIPs so that the Unix mode of each entry is under the test's control.
#include "check.h"
#include "package.h"
#include "posix/files_posix.h"
#include "sha256.h"
#include "zip_fixture.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
using namespace uc::update;
using Json = nlohmann::json;
namespace {
using zipfix::Entry;
using zipfix::zip_bytes;
fs::path scratch() {
    auto path = fs::temp_directory_path() / ("uc-package-posix-" + std::to_string(::getpid()));
    fs::remove_all(path);
    fs::create_directories(path);
    return path;
}
std::vector<Entry> base_files() {
    return {{"uc", "game-binary"}, {"uc_launcher", "launcher-binary"}, {"tools/wit/wit", "wit"}, {"data/uc.map", "map"},
            {"assets/launcher/graphite-train.png", "train"}};
}
std::string manifest_for(const std::vector<Entry>& files, const std::string& version = "0.0.2") {
    Json files_json = Json::array();
    for (const auto& f : files) files_json.push_back({{"path", f.name}, {"size", f.data.size()}, {"sha256", sha256_hex(f.data)}});
    return Json{{"format", 1}, {"version", version}, {"helper_files", Json::array()}, {"files", files_json}}.dump();
}
struct Case { fs::path root, zip, staging; Release release; };
Case make(const fs::path& root, const std::string& name, std::vector<Entry> files, std::optional<std::string> manifest = {}, const std::string& version = "0.0.2") {
    auto all = files;
    all.push_back({"release.json", manifest ? *manifest : manifest_for(files, version)});
    Case c{root, root / (name + ".zip"), root / (name + "-stage"), {}};
    const auto bytes = zip_bytes(all);
    std::ofstream(c.zip, std::ios::binary) << bytes;
    c.release = Release{Version{0, 0, 2}, "https://example.invalid/x.zip", "", sha256_hex(bytes), bytes.size()};
    return c;
}
bool fails(const Result<Package>& r, ErrorCode code) { return failed(r) && std::get<Error>(r).code == code; }
}
int main() {
    const auto root = scratch();
    {   // a good package extracts, verifies, and the known executables are 0755 whatever the archive said
        auto files = base_files();
        files[0].mode = 0100644;                                  // stored without the exec bit
        auto c = make(root, "good", files);
        auto result = prepare_package(c.zip, c.release, c.staging);
        CHECK(!failed(result));
        const auto& package = std::get<Package>(result);
        CHECK(package.manifest.version == (Version{0, 0, 2}) && package.manifest.files.size() == 5);
        struct stat info{};
        for (const char* exe : {"uc", "uc_launcher", "tools/wit/wit"}) {
            CHECK(::stat((c.staging / exe).c_str(), &info) == 0 && (info.st_mode & 0777) == 0755);
        }
        CHECK(::stat((c.staging / "data/uc.map").c_str(), &info) == 0 && (info.st_mode & 0777) == 0644);
        CHECK(!failed(verify_package(package)));
        CHECK(std::get<std::string>(read_text(c.staging / "uc", 100)) == "game-binary");
        std::ofstream(c.staging / "extra.txt") << "x";            // an unexpected file is noticed by verify_package
        CHECK(failed(verify_package(package)));
    }
    {   // archive-level mismatches
        auto c = make(root, "size", base_files());
        auto release = c.release; release.size += 1;
        CHECK(fails(prepare_package(c.zip, release, root / "size-stage"), ErrorCode::integrity));
        release = c.release; release.sha256 = std::string(64, '0');
        CHECK(fails(prepare_package(c.zip, release, root / "size-stage"), ErrorCode::integrity));
        release = c.release; release.version = Version{0, 0, 3};
        CHECK(failed(prepare_package(c.zip, release, root / "size-stage")));   // manifest says 0.0.2
        CHECK(!fs::exists(root / "size-stage"));                              // nothing is left behind
    }
    {   // paths the manifest or archive must not carry
        for (const char* bad : {"../evil", "/abs", "a/../../b", "saves/x", "game/DATA/sys/main.dol", "config.json", "launcher-state.json", "a//b", "a\\b"}) {
            auto files = base_files(); files.push_back({bad, "x"});
            auto c = make(root, "path", files);
            CHECK(failed(prepare_package(c.zip, c.release, root / "path-stage")));
            CHECK(!fs::exists(root / "path-stage"));
            fs::remove(c.zip);
        }
    }
    {   // symlink and directory entries
        auto files = base_files(); files.push_back({"link", "uc", 0120777});
        auto c = make(root, "link", files);
        CHECK(failed(prepare_package(c.zip, c.release, root / "link-stage")));
        files = base_files(); files.push_back({"dir/", "", 040755});
        c = make(root, "dir", files);
        CHECK(failed(prepare_package(c.zip, c.release, root / "dir-stage")));
    }
    {   // the archive and the manifest must list the same files with the same bytes
        auto files = base_files();
        auto manifest = manifest_for(files);
        files.push_back({"extra.bin", "x"});                       // in the zip, not in the manifest
        CHECK(failed(prepare_package(make(root, "extra", files, manifest).zip, make(root, "extra", files, manifest).release, root / "extra-stage")));
        files = base_files();
        auto full = files;
        full.push_back({"missing.bin", "x"});                      // in the manifest, not in the zip
        auto c = make(root, "missing", files, manifest_for(full));
        CHECK(failed(prepare_package(c.zip, c.release, root / "missing-stage")));
        auto wrong = base_files();
        auto json = Json::parse(manifest_for(wrong)); json["files"][0]["sha256"] = std::string(64, 'b');
        c = make(root, "hash", wrong, json.dump());
        CHECK(failed(prepare_package(c.zip, c.release, root / "hash-stage")));
        CHECK(!fs::exists(root / "hash-stage"));
        auto no_launcher = base_files(); no_launcher.erase(no_launcher.begin() + 1);
        c = make(root, "nolauncher", no_launcher);
        CHECK(failed(prepare_package(c.zip, c.release, root / "nolauncher-stage")));
    }
    {   // cancel and an existing staging folder
        auto c = make(root, "cancel", base_files());
        std::stop_source stop; stop.request_stop();
        CHECK(fails(prepare_package(c.zip, c.release, root / "cancel-stage", stop.get_token()), ErrorCode::cancelled));
        CHECK(!fs::exists(root / "cancel-stage"));
        fs::create_directories(root / "occupied");
        CHECK(failed(prepare_package(c.zip, c.release, root / "occupied")));
        CHECK(fs::exists(root / "occupied"));                      // not ours: left alone
    }
    {   // validate_target_path: missing parents are fine, a symlinked parent or target is not
        const auto install = root / "install";
        fs::create_directories(install / "data");
        fs::create_directories(root / "elsewhere");
        fs::create_directory_symlink(root / "elsewhere", install / "tools");
        std::ofstream(install / "uc") << "x";
        fs::create_symlink(install / "uc", install / "uc_link");
        CHECK(!failed(validate_target_path(install, "uc")));
        CHECK(!failed(validate_target_path(install, "data/new/deep/file.bin")));
        CHECK(failed(validate_target_path(install, "tools/wit/wit")));
        CHECK(failed(validate_target_path(install, "uc_link")));
        CHECK(failed(validate_target_path(install, "data")));          // a directory where a file is expected
        CHECK(failed(validate_target_path(install, "../out")));
    }
    CHECK(std::get<std::string>(sha256_file(root / "good.zip")).size() == 64);
    CHECK(failed(sha256_file(root / "nonexistent")));
    fs::remove_all(root);
    std::puts("update package posix: passed");
}
