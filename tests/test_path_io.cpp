// Paths with spaces and non-ASCII letters, the executable's own path and the per-user data folder work the same on both platforms.
#include "check.h"
#include "path_io.h"
#include <cstdlib>
#include <filesystem>
#include <string>

int main() {
    namespace fs = std::filesystem;
    fs::path folder(u8"uc path io тест "); folder += std::to_string(std::rand());
    const fs::path root = fs::temp_directory_path() / folder;
    fs::create_directories(root);
    const fs::path file = root / u8"файл с пробелом.txt";
    {
        std::FILE* out = uc::fopen_path(file, "wb");
        CHECK(out != nullptr);
        std::fputs("round trip", out);
        std::fclose(out);
    }
    {
        std::FILE* in = uc::fopen_path(file, "rb");
        CHECK(in != nullptr);
        char text[32]{};
        CHECK(std::fread(text, 1, sizeof text - 1, in) == 10 && std::string(text) == "round trip");
        std::fclose(in);
    }
    CHECK(uc::fopen_path(root / u8"нет такого", "rb") == nullptr);
    fs::remove_all(root);

    const fs::path exe = uc::exe_path();
    CHECK(exe.is_absolute() && fs::exists(exe));
    CHECK(exe.stem().string().find("test_path_io") != std::string::npos);

#ifdef __linux__
    setenv("XDG_DATA_HOME", "/tmp/xdg data", 1);
    CHECK(uc::user_data_dir() == fs::path("/tmp/xdg data/umbrella-chronicles"));
    unsetenv("XDG_DATA_HOME");
    setenv("HOME", "/home/someone", 1);
    CHECK(uc::user_data_dir() == fs::path("/home/someone/.local/share/umbrella-chronicles"));
#elif defined(__APPLE__)
    setenv("HOME", "/Users/someone", 1);
    CHECK(uc::user_data_dir() == fs::path("/Users/someone/Library/Application Support/Umbrella Chronicles"));
#else
    CHECK(uc::user_data_dir() == exe.parent_path());   // Windows keeps everything next to the executable
#endif
    return 0;
}
