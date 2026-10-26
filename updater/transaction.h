#pragma once
#include "package.h"
namespace uc::update {
struct InstallOperation {
    std::string path;
    bool existed{};
    uint64_t old_size{};
    std::string old_sha256;
    std::optional<FileEntry> replacement;
};
struct InstallPlan {
    std::filesystem::path install_root, operation_root;
    Package package;
    Manifest old_manifest;
    std::vector<InstallOperation> operations;
};
Result<InstallPlan> plan_install(const std::filesystem::path& install,const Package& package);
Status apply_install(const InstallPlan& plan);
Status finish_install(const std::filesystem::path& operation);
Status recover_install(const std::filesystem::path& install,const std::filesystem::path& operation);
Result<bool> pending_install(const std::filesystem::path& install,const std::filesystem::path& operation);
}
