// Linux/Deck only (tests/linux/CMakeLists.txt): the POSIX twin of test_update_transaction.cpp. Crash points are real: a forked child _exits at the Nth checkpoint.
#include "check.h"
#include "transaction.h"
#include "posix/files_posix.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <map>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace uc::update;
using Json = nlohmann::json;
static unsigned checkpoints = 0, crash_at = 0;
static std::string fail_on;
namespace uc::update::detail {
Status checkpoint(std::string_view name) {
    ++checkpoints;
    if (crash_at && checkpoints == crash_at) ::_exit(99);
    if (name == fail_on) return Error{ErrorCode::no_space, "injected disk full"};
    return std::monostate{};
}
}
template<class T> T ok(Result<T> value) {
    if (failed(value)) std::fprintf(stderr, "%s\n", std::get<Error>(value).detail.c_str());
    CHECK(!failed(value)); return std::get<T>(std::move(value));
}
void pass(Status status) { if (failed(status)) std::fprintf(stderr, "%s\n", std::get<Error>(status).detail.c_str()); CHECK(!failed(status)); }
void write(const fs::path& path, std::string_view data, mode_t mode = 0644) {
    fs::create_directories(path.parent_path()); { std::ofstream f(path, std::ios::binary); f.write(data.data(), data.size()); CHECK(f.good()); }
    ::chmod(path.c_str(), mode);
}
std::string read(const fs::path& path) { std::ifstream f(path, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
Json make_manifest(const fs::path& root, std::string version, std::vector<std::string> names) {
    Json files = Json::array();
    for (const auto& name : names) files.push_back({{"path", name}, {"size", fs::file_size(root / name)}, {"sha256", ok(sha256_file(root / name))}});
    return {{"format", 1}, {"version", version}, {"files", files}, {"helper_files", Json::array()}};
}
std::map<std::string, std::string> tree(const fs::path& root) {   // path -> mode + content, so a lost exec bit is noticed
    std::map<std::string, std::string> result;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_directory() && it->path().filename() == ".uc-update") { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file()) {
            struct stat info{}; ::stat(it->path().c_str(), &info);
            result[it->path().lexically_relative(root).generic_string()] = std::to_string(info.st_mode & 0777) + ":" + read(it->path());
        }
    }
    return result;
}
const char* kId = "11111111-1111-4111-8111-111111111111";
struct Fixture {
    fs::path root, operation, stage;
    Package package;
    Fixture() {
        static unsigned serial = 0;
        root = fs::temp_directory_path() / ("uc-transaction-posix-" + std::to_string(::getpid()) + "-" + std::to_string(++serial));
        fs::remove_all(root);
        operation = root / ".uc-update" / kId; stage = operation / "stage";
        fs::create_directories(stage);
        for (auto name : {"uc", "uc_launcher"}) { write(root / name, "OLD", 0755); write(stage / name, "NEW", 0755); }
        write(root / "obsolete.txt", "OLD"); write(stage / "added.txt", "NEW");
        write(root / "saves" / "user.dat", "USER"); write(root / "mods" / "local.lua", "USER"); write(root / "config.json", "USER");
        write(root / "data" / "pipeline_cache.bin", "CACHE"); write(root / "unknown.txt", "UNKNOWN");
        write(root / "release.json", make_manifest(root, "0.0.1", {"uc", "uc_launcher", "obsolete.txt"}).dump());
        write(stage / "release.json", make_manifest(stage, "0.0.2", {"uc", "uc_launcher", "added.txt"}).dump());
        package = {stage, ok(parse_manifest(read(stage / "release.json"), Platform::steamdeck))};
    }
    ~Fixture() { ::chmod(root.c_str(), 0755); std::error_code ec; fs::remove_all(root, ec); }
};
int run_child(const fs::path& root, unsigned crash, bool recover) {   // the child dies at the crash-th checkpoint with exit code 99
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        crash_at = crash; checkpoints = 0;
        auto operation = root / ".uc-update" / kId;
        if (recover) { pass(recover_install(root, operation)); ::_exit(0); }
        Package package{operation / "stage", ok(parse_manifest(read(operation / "stage" / "release.json"), Platform::steamdeck))};
        pass(apply_install(ok(plan_install(root, package)))); ::_exit(0);
    }
    int status = 0; CHECK(::waitpid(pid, &status, 0) == pid); return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
int main() {
    unsigned checkpoint_count = 0, recovery_checkpoints = 0;
    {   // update replaces, adds and removes; user data stays; recovery restores everything, twice
        Fixture f; auto before = tree(f.root); auto plan = ok(plan_install(f.root, f.package)); checkpoints = 0;
        pass(apply_install(plan)); checkpoint_count = checkpoints;
        CHECK(read(f.root / "uc") == "NEW" && read(f.root / "added.txt") == "NEW" && !fs::exists(f.root / "obsolete.txt"));
        CHECK(read(f.root / "saves" / "user.dat") == "USER" && read(f.root / "data" / "pipeline_cache.bin") == "CACHE" && read(f.root / "unknown.txt") == "UNKNOWN");
        struct stat info{}; CHECK(::stat((f.root / "uc").c_str(), &info) == 0 && (info.st_mode & 0777) == 0755);   // the exec bit travels with the file
        checkpoints = 0; pass(recover_install(f.root, f.operation)); recovery_checkpoints = checkpoints; CHECK(tree(f.root) == before);
        pass(recover_install(f.root, f.operation)); CHECK(tree(f.root) == before);
    }
    {   // finish_install commits and drops the backup; recovery afterwards changes nothing
        Fixture f; pass(apply_install(ok(plan_install(f.root, f.package))));
        pass(finish_install(f.operation)); CHECK(!fs::exists(f.operation / "backup")); auto committed = tree(f.root);
        pass(recover_install(f.root, f.operation)); CHECK(tree(f.root) == committed);
    }
    {   // an unmanaged file on a release path is backed up, replaced, and restored by rollback
        Fixture f; write(f.root / "added.txt", "USER"); write(f.root / "uc", "EDITED", 0755); auto before = tree(f.root);
        pass(apply_install(ok(plan_install(f.root, f.package))));
        CHECK(read(f.root / "added.txt") == "NEW" && read(f.root / "unknown.txt") == "UNKNOWN");
        pass(recover_install(f.root, f.operation)); CHECK(tree(f.root) == before);
    }
    {   // a tampered old manifest naming a protected path is refused
        Fixture f; auto j = Json::parse(read(f.root / "release.json")); j["files"][0]["path"] = "config.json";
        write(f.root / "release.json", j.dump()); CHECK(failed(plan_install(f.root, f.package)));
    }
    {   // a missing installed file: rollback restores the absence; a directory on a release path refuses the plan
        Fixture f; fs::remove(f.root / "uc"); auto before = tree(f.root);
        pass(apply_install(ok(plan_install(f.root, f.package)))); pass(recover_install(f.root, f.operation)); CHECK(tree(f.root) == before);
        Fixture g; fs::remove(g.root / "uc"); fs::create_directory(g.root / "uc"); CHECK(failed(plan_install(g.root, g.package)));
    }
    {   // a symlink on a release path is refused
        Fixture f; fs::remove(f.root / "uc"); fs::create_symlink(f.root / "unknown.txt", f.root / "uc"); CHECK(failed(plan_install(f.root, f.package)));
    }
    {   // an unwritable install folder fails before any change
        Fixture f; auto before = tree(f.root); auto plan = ok(plan_install(f.root, f.package));
        ::chmod(f.root.c_str(), 0555); auto status = apply_install(plan); ::chmod(f.root.c_str(), 0755);
        CHECK(failed(status)); CHECK(tree(f.root) == before);
    }
    {   // disk full at a checkpoint: nothing is lost and recovery restores the original
        Fixture f; auto before = tree(f.root); auto plan = ok(plan_install(f.root, f.package)); fail_on = "before_replace";
        CHECK(failed(apply_install(plan))); fail_on.clear(); pass(recover_install(f.root, f.operation)); CHECK(tree(f.root) == before);
    }
    {   // a staged file changed after planning
        Fixture f; auto plan = ok(plan_install(f.root, f.package)); write(f.stage / "uc", "TAMPERED", 0755);
        CHECK(failed(apply_install(plan))); CHECK(read(f.root / "uc") == "OLD");
    }
    {   // a tampered journal path is refused, the intact journal recovers
        Fixture f; auto before = tree(f.root); pass(apply_install(ok(plan_install(f.root, f.package))));
        auto header = read(f.operation / "journal.json"); auto j = Json::parse(header); j["operations"][0]["path"] = "../outside";
        write(f.operation / "journal.json", j.dump()); CHECK(failed(recover_install(f.root, f.operation)));
        write(f.operation / "journal.json", header); pass(recover_install(f.root, f.operation)); CHECK(tree(f.root) == before);
    }
    CHECK(checkpoint_count > 5);
    {   // a torn final journal line is repaired
        Fixture f; auto before = tree(f.root); pass(apply_install(ok(plan_install(f.root, f.package))));
        { std::ofstream log(f.operation / "journal.log", std::ios::app); log << "{\"event\":\"rest"; }
        pass(recover_install(f.root, f.operation)); CHECK(tree(f.root) == before);
        pass(recover_install(f.root, f.operation)); CHECK(tree(f.root) == before);
    }
    {   // a user's file at our temporary name stops the install; one created after a crash survives recovery
        Fixture f; auto plan = ok(plan_install(f.root, f.package));
        write(f.root / (std::string(".uc-new-") + kId + "-0"), "USER TEMP");
        auto before = tree(f.root); CHECK(failed(apply_install(plan))); CHECK(tree(f.root) == before);
        Fixture g; pass(apply_install(ok(plan_install(g.root, g.package))));
        auto temporary = g.root / (std::string(".uc-new-") + kId + "-0"); write(temporary, "USER FILE AFTER CRASH");
        pass(recover_install(g.root, g.operation)); CHECK(read(temporary) == "USER FILE AFTER CRASH" && read(g.root / "uc") == "OLD");
    }
    for (unsigned checkpoint = 1; checkpoint <= checkpoint_count; ++checkpoint) {   // kill the installer at every checkpoint
        Fixture f; auto before = tree(f.root); CHECK(run_child(f.root, checkpoint, false) == 99);
        pass(recover_install(f.root, f.operation)); CHECK(tree(f.root) == before);
        pass(recover_install(f.root, f.operation)); CHECK(tree(f.root) == before);
    }
    CHECK(recovery_checkpoints > 0);
    for (unsigned checkpoint = 1; checkpoint <= recovery_checkpoints; ++checkpoint) {   // and the recovery itself
        Fixture f; auto before = tree(f.root); pass(apply_install(ok(plan_install(f.root, f.package))));
        CHECK(run_child(f.root, checkpoint, true) == 99);
        pass(recover_install(f.root, f.operation)); CHECK(tree(f.root) == before);
        pass(recover_install(f.root, f.operation)); CHECK(tree(f.root) == before);
    }
    std::printf("update transaction posix: rollback, preservation and %u crash checkpoints passed\n", checkpoint_count);
}
