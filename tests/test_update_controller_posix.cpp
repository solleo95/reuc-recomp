// Linux/Deck only (tests/linux/CMakeLists.txt): UpdateController with a fake transport on a temporary install.
#include "check.h"
#include "update.h"
#include "posix/files_posix.h"
#include "sha256.h"
#include "zip_fixture.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace uc::update;
using namespace uc::launcher;
using Json = nlohmann::json;
namespace {
std::string read(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
void write(const fs::path& p, std::string_view data) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << data; }
std::string manifest_text(const std::vector<zipfix::Entry>& files, const std::string& version) {
    Json list = Json::array();
    for (const auto& f : files) list.push_back({{"path", f.name}, {"size", f.data.size()}, {"sha256", sha256_hex(f.data)}});
    return Json{{"format", 1}, {"version", version}, {"helper_files", Json::array()}, {"files", list}}.dump();
}
struct Fake : Transport {
    std::atomic<unsigned> checks{0}, status{200};
    std::atomic<bool> hold_download{false};
    std::string metadata, bytes;
    Result<HttpResponse> get_metadata(std::stop_token) override { ++checks; return HttpResponse{status.load(), metadata}; }
    Status download(const Release&, const DownloadSink& sink, std::stop_token stop) override {
        auto status = sink(std::span<const char>(bytes.data(), bytes.size() / 2));
        if (failed(status)) return status;
        while (hold_download && !stop.stop_requested()) ::usleep(1000);
        if (stop.stop_requested()) return Error{ErrorCode::cancelled, "cancelled"};
        return sink(std::span<const char>(bytes.data() + bytes.size() / 2, bytes.size() - bytes.size() / 2));
    }
};
struct World {
    fs::path root;
    std::string zip, release_json;
    Manifest installed;
    std::string new_version;
    explicit World(const std::string& version_in_zip = "0.0.2", bool tamper = false) : new_version(version_in_zip) {
        static unsigned serial = 0;
        root = fs::temp_directory_path() / ("uc-controller-posix-" + std::to_string(::getpid()) + "-" + std::to_string(++serial));
        fs::remove_all(root);
        std::vector<zipfix::Entry> old_files = {{"uc", "OLD"}, {"uc_launcher", "OLD"}, {"gone.txt", "OLD"}};
        std::vector<zipfix::Entry> new_files = {{"uc", "NEW"}, {"uc_launcher", "NEW"}, {"added.txt", "NEW"}};
        for (const auto& f : old_files) write(root / f.name, f.data);
        write(root / "saves/user.dat", "USER");
        write(root / "release.json", manifest_text(old_files, "0.0.1"));
        auto all = new_files;
        all.push_back({"release.json", manifest_text(new_files, version_in_zip)});
        if (tamper) all[0].data = "XXX";            // the content no longer matches the manifest's hash
        zip = zipfix::zip_bytes(all);
        release_json = Json{{"tag_name", "v" + version_in_zip}, {"draft", false}, {"prerelease", false}, {"body", "Notes"},
            {"assets", Json::array({{{"name", "UC-SteamDeck-" + version_in_zip + ".zip"}, {"state", "uploaded"}, {"size", zip.size()},
                {"digest", "sha256:" + sha256_hex(zip)},
                {"browser_download_url", "https://github.com/solleo95/reuc-recomp/releases/download/v" + version_in_zip + "/UC-SteamDeck-" + version_in_zip + ".zip"}}})}}.dump();
        installed = std::get<Manifest>(parse_manifest(read(root / "release.json"), Platform::steamdeck));
    }
    ~World() { std::error_code ec; fs::remove_all(root, ec); }
    std::unique_ptr<Fake> transport() { auto t = std::make_unique<Fake>(); t->bytes = zip; t->metadata = release_json; return t; }
    size_t leftovers() const { return fs::exists(root / ".uc-update") ? std::distance(fs::directory_iterator(root / ".uc-update"), fs::directory_iterator{}) : 0; }
};
void settle(UpdateController& c) {
    for (int i = 0; i < 5000 && c.busy(); ++i) { c.poll(); ::usleep(1000); }
    c.poll();
    CHECK(!c.busy());
}
}
int main() {
    {   // no installed manifest: nothing is checked
        World w; auto t = w.transport(); auto* fake = t.get(); UpdateController c(w.root, std::move(t));
        c.start(std::nullopt); c.start(w.installed); CHECK(fake->checks == 0 && c.state() == UpdateState::disabled);
    }
    for (unsigned status : {404u, 403u, 429u, 500u}) {   // network or GitHub trouble never blocks Play and shows no window
        World w; auto t = w.transport(); t->status = status; UpdateController c(w.root, std::move(t));
        c.start(w.installed); settle(c);
        CHECK(c.state() == UpdateState::disabled && !c.blocks_game_launch() && !c.close_requested());
    }
    {   // the archive is not newer than what is installed: no offer (a carried-over archive)
        World w("0.0.1"); UpdateController c(w.root, w.transport()); c.start(w.installed); settle(c);
        CHECK(c.state() == UpdateState::disabled);
    }
    {   // a newer archive is offered; while the game runs accept refuses; then it installs and waits for the new launcher
        World w; UpdateController c(w.root, w.transport()); c.start(w.installed); settle(c);
        CHECK(c.state() == UpdateState::available && c.release() && c.release()->version == (Version{0, 0, 2}) && c.release()->notes == "Notes");
        c.accept(true); CHECK(c.state() == UpdateState::available && c.error() && c.error()->code == ErrorCode::busy);
        c.accept(); settle(c);
        CHECK(c.state() == UpdateState::ready_to_close && c.close_requested() && c.blocks_game_launch());
        CHECK(read(w.root / "uc") == "NEW" && read(w.root / "added.txt") == "NEW" && !fs::exists(w.root / "gone.txt") && read(w.root / "saves/user.dat") == "USER");
        const auto operation = c.applied_operation();
        CHECK(std::get<bool>(pending_install(w.root, operation)));           // not committed until the new launcher confirms
        c.handoff();
        ::setenv("UC_UPDATE_OPERATION", operation.filename().c_str(), 1);    // what restart_after_update leaves for the new process
        auto confirm = recover_pending_update(w.root).confirm;
        CHECK(confirm && *confirm == operation && read(w.root / "uc") == "NEW");
        CHECK(!failed(finish_install(*confirm)) && !std::get<bool>(pending_install(w.root, operation)));
        ::unsetenv("UC_UPDATE_OPERATION");
    }
    {   // an update nobody confirmed (crash, power cut) is rolled back by the next start
        World w; {
            UpdateController c(w.root, w.transport()); c.start(w.installed); settle(c); c.accept(); settle(c);
            CHECK(c.close_requested()); c.handoff();
        }
        ::unsetenv("UC_UPDATE_OPERATION");
        const auto recovery = recover_pending_update(w.root);
        CHECK(!recovery.confirm && recovery.problem.empty());
        CHECK(read(w.root / "uc") == "OLD" && read(w.root / "gone.txt") == "OLD" && !fs::exists(w.root / "added.txt"));
        CHECK(read(w.root / "release.json") == manifest_text({{"uc", "OLD"}, {"uc_launcher", "OLD"}, {"gone.txt", "OLD"}}, "0.0.1"));
    }
    {   // a rollback that cannot be done (the install folder was moved after a crash: the journal names the old place) is reported to the user, not only logged
        World w; {
            UpdateController c(w.root, w.transport()); c.start(w.installed); settle(c); c.accept(); settle(c); c.handoff();
        }
        ::unsetenv("UC_UPDATE_OPERATION");
        const fs::path moved = w.root.string() + "-moved";
        fs::rename(w.root, moved);
        const auto recovery = recover_pending_update(moved);
        CHECK(!recovery.confirm && !recovery.problem.empty());
        CHECK(read(moved / "uc") == "NEW");                                      // nothing was changed on the way
        fs::rename(moved, w.root);
    }
    {   // cancelling after the files were installed (the Cancel button is shown through "verifying", which includes the install) puts the old version back and
        // leaves the update offered: it is not committed behind the user's back, and pressing Update again works
        World w; UpdateController c(w.root, w.transport()); c.start(w.installed); settle(c); c.accept(); settle(c);
        CHECK(c.close_requested() && read(w.root / "uc") == "NEW");
        c.cancel();
        CHECK(c.state() == UpdateState::available && !c.close_requested());
        CHECK(read(w.root / "uc") == "OLD" && read(w.root / "gone.txt") == "OLD" && !fs::exists(w.root / "added.txt") && w.leftovers() == 0);
        CHECK(read(w.root / "release.json") == manifest_text({{"uc", "OLD"}, {"uc_launcher", "OLD"}, {"gone.txt", "OLD"}}, "0.0.1"));
        c.accept(); settle(c);
        CHECK(c.state() == UpdateState::ready_to_close && read(w.root / "uc") == "NEW");
    }
    {   // a broken archive: the old install stays, the staging is cleaned, the user can retry
        World w("0.0.2", true); UpdateController c(w.root, w.transport()); c.start(w.installed); settle(c);
        CHECK(c.state() == UpdateState::available);
        c.accept(); settle(c);
        CHECK(c.state() == UpdateState::failed && c.error());
        CHECK(read(w.root / "uc") == "OLD" && w.leftovers() == 0 && !c.close_requested() && !c.blocks_game_launch());
    }
    {   // cancel during the download leaves nothing behind
        World w; auto t = w.transport(); t->hold_download = true; auto* fake = t.get(); UpdateController c(w.root, std::move(t));
        c.start(w.installed); settle(c); c.accept();
        for (int i = 0; i < 2000 && c.downloaded() == 0; ++i) { c.poll(); ::usleep(1000); }
        CHECK(c.downloaded() > 0 && c.state() == UpdateState::downloading);
        c.cancel(); fake->hold_download = false; settle(c);
        CHECK(c.state() == UpdateState::available && w.leftovers() == 0 && read(w.root / "uc") == "OLD");
    }
    {   // the restart runs the launcher file at the given path (the one the update just installed: /proc/self/exe would still be the replaced inode) and names the operation to confirm
        World w; const auto marker = w.root / "marker.txt";
        write(w.root / "uc_launcher", "#!/bin/sh\necho \"$UC_UPDATE_OPERATION\" > \"$1\"\n");
        ::chmod((w.root / "uc_launcher").c_str(), 0755);
        const auto launcher = (w.root / "uc_launcher").string(), argument = marker.string();
        const pid_t child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            char* argv[] = {const_cast<char*>("uc_launcher"), const_cast<char*>(argument.c_str()), nullptr};
            restart_launcher(launcher, "op-123", argv);
            ::_exit(97);   // only reached when the exec failed
        }
        int status = 0; CHECK(::waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(read(marker) == "op-123\n");
    }
    std::puts("update controller posix: passed");
}
