#pragma once
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uc::update {
enum class ErrorCode { invalid_version, invalid_manifest, invalid_release, unsafe_path,
    invalid_archive, integrity, network, rate_limited, cancelled, busy, permission,
    no_space, io, startup, recovery_required };
struct Error { ErrorCode code; std::string detail; };
template<class T> using Result = std::variant<T, Error>;
using Status = Result<std::monostate>;
template<class T> bool failed(const Result<T>& result) { return std::holds_alternative<Error>(result); }
struct Version {
    uint16_t major{}, minor{}, patch{};
    auto operator<=>(const Version&) const = default;
};
struct FileEntry { std::string path; uint64_t size{}; std::string sha256; };
struct Manifest { Version version; std::vector<FileEntry> files; std::vector<std::string> helper_files; };
struct Release { Version version; std::string asset_url, notes, sha256; uint64_t size{}; };
inline constexpr uint64_t max_zip_size = 2ull << 30;
inline constexpr uint64_t max_package_size = 8ull << 30;
inline constexpr size_t max_entries = 50000;
inline constexpr size_t max_manifest_size = 16u << 20;
inline constexpr size_t max_metadata_size = 1u << 20;
Result<Version> parse_version(std::string_view text, bool allow_v_prefix = false);
std::string format_version(Version version);
Status validate_managed_path(std::string_view relative);
Result<std::wstring> path_key(std::string_view relative);
Result<Manifest> parse_manifest(std::string_view json);
Result<std::optional<Release>> select_release(std::string_view json, Version installed);
}
