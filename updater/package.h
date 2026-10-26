#pragma once
#include "release.h"
#include <filesystem>
#include <stop_token>
namespace uc::update {
struct Package { std::filesystem::path root; Manifest manifest; };
Result<std::string> sha256_file(const std::filesystem::path& file);
Result<Package> prepare_package(const std::filesystem::path& zip, const Release& release,
    const std::filesystem::path& staging, std::stop_token stop = {});
Status verify_package(const Package& package);
Status validate_target_path(const std::filesystem::path& install, std::string_view relative);
}
