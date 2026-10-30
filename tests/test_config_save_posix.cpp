// save_config on Linux: a new file, a replaced file, a stale writer lock, a file changed behind the editor's back; no lock or temporary file is left behind.
#ifndef __linux__
int main() { return 0; }
#else
#include "check.h"
#include "input_config.h"
#include "test_pid.h"
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static std::string directory_listing(const fs::path& dir) {
    std::string names;
    for (const auto& entry : fs::directory_iterator(dir)) names += entry.path().filename().string() + ";";
    return names;
}
static bool throws_with(const fs::path& path, uc::ConfigDocument& document, const char* text) {
    try { uc::save_config(path, document); } catch (const std::exception& error) { return std::string(error.what()).find(text) != std::string::npos; }
    return false;
}

int main() {
    fs::path folder(u8"uc config тест "); folder += std::to_string(test_process_id());
    const fs::path dir = fs::temp_directory_path() / folder;
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path path = dir / "config.json";

    auto document = uc::load_config(path);                    // no file yet: defaults
    CHECK(!document.original_bytes);
    uc::save_config(path, document);                          // creates it
    CHECK(fs::exists(path) && directory_listing(dir) == "config.json;");

    document = uc::load_config(path);
    CHECK(document.original_bytes.has_value());
    document.properties["note"] = "второе сохранение";
    uc::save_config(path, document);                          // replaces it atomically
    CHECK(uc::load_config(path).properties["note"] == "второе сохранение");
    CHECK(directory_listing(dir) == "config.json;");

    { std::ofstream lock(dir / "config.json.write-lock"); lock << "x"; }   // another writer is active
    CHECK(throws_with(path, document, "writer lock unavailable"));
    CHECK(fs::exists(dir / "config.json.write-lock"));        // a lock this writer did not create stays
    fs::remove(dir / "config.json.write-lock");

    { std::ofstream changed(path, std::ios::binary | std::ios::trunc); changed << "{}"; }   // edited by hand after loading
    CHECK(throws_with(path, document, "file changed since loading"));
    CHECK(directory_listing(dir) == "config.json;");
    fs::remove_all(dir);
    return 0;
}
#endif
