#include "release.h"
#include <windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <set>

namespace uc::update {
namespace {
using Json = nlohmann::json;
Error invalid(ErrorCode code, std::string text) { return {code, std::move(text)}; }
bool hash_valid(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool unsigned_size(const Json& value, uint64_t limit) {
    return value.is_number_unsigned() && value.get<uint64_t>() <= limit;
}
bool reserved_component(const std::wstring& component) {
    auto name = component.substr(0, component.find(L'.'));
    if (name == L"CON" || name == L"PRN" || name == L"AUX" || name == L"NUL" ||
        name == L"CONIN$" || name == L"CONOUT$") return true;
    if (name.size() == 4 && (name.starts_with(L"COM") || name.starts_with(L"LPT")))
        return (name[3] >= L'0' && name[3] <= L'9') ||
            name[3] == 0x00b9 || name[3] == 0x00b2 || name[3] == 0x00b3;
    return false;
}
}
Result<Version> parse_version(std::string_view text, bool allow_v_prefix) {
    if (allow_v_prefix && text.starts_with('v')) text.remove_prefix(1);
    std::array<uint16_t, 3> values{};
    for (size_t i = 0; i < values.size(); ++i) {
        const auto end = text.find('.');
        const auto part = text.substr(0, end);
        unsigned value = 0;
        if (part.empty() || part.size() > 5 || (part.size() > 1 && part.front() == '0') ||
            !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return invalid(ErrorCode::invalid_version, "Expected X.Y.Z (components 0..65535)");
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
        if (parsed.ec != std::errc{} || value > 65535 || ((i == 2) != (end == text.npos)))
            return invalid(ErrorCode::invalid_version, "Expected three version components");
        values[i] = static_cast<uint16_t>(value);
        if (end != text.npos) text.remove_prefix(end + 1);
    }
    return Version{values[0], values[1], values[2]};
}
std::string format_version(Version value) {
    return std::to_string(value.major) + "." + std::to_string(value.minor) + "." + std::to_string(value.patch);
}
Result<std::wstring> path_key(std::string_view relative) {
    if (relative.empty() || relative.size() > 32760 || relative.find('\0') != relative.npos)
        return invalid(ErrorCode::unsafe_path, "Empty or oversized relative path");
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, relative.data(),
        static_cast<int>(relative.size()), nullptr, 0);
    if (!count) return invalid(ErrorCode::unsafe_path, "Path is not valid UTF-8");
    std::wstring wide(count, L'\0'), key(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, relative.data(), static_cast<int>(relative.size()), wide.data(), count);
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, wide.data(), count, key.data(), count, nullptr, nullptr, 0))
        return invalid(ErrorCode::unsafe_path, "Cannot normalize path");
    for (wchar_t c : key)
        if (c < 32 || c == 127 || c == L'\\' || c == L':' || c == L'~' ||
            c == L'<' || c == L'>' || c == L'"' || c == L'|' || c == L'?' || c == L'*')
            return invalid(ErrorCode::unsafe_path, "Invalid Windows path character");
    size_t start = 0;
    do {
        auto end = key.find(L'/', start);
        auto part = key.substr(start, end == key.npos ? key.size() - start : end - start);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' ||
            part.back() == L' ' || reserved_component(part))
            return invalid(ErrorCode::unsafe_path, "Invalid Windows path component");
        if (end == key.npos) break;
        start = end + 1;
    } while (start <= key.size());
    return key;
}
Status validate_managed_path(std::string_view relative) {
    auto key = path_key(relative);
    if (failed(key)) return std::get<Error>(key);
    const auto& path = std::get<std::wstring>(key);
    const auto root = path.substr(0, path.find(L'/'));
    if(root.starts_with(L"CONFIG.JSON.") || root.starts_with(L"CPU-PROFILE.JSON.") ||
        root==L"NVNGX_DLSSNR.DLL" || root==L"CPU-SAMPLES.TXT" ||
        path==L"DATA/PIPELINE_KEYS.BIN" || path==L"DATA/PIPELINE_CACHE.BIN" || path==L"DATA/PIPELINE_CACHE.TMP")
        return invalid(ErrorCode::unsafe_path, "Protected user runtime file");
    for (auto reserved : {L"CONFIG.JSON", L"CPU-PROFILE.JSON", L"SAVES", L"MODS", L"TEXTURES",
                         L"GAME", L"DEBUG", L"LOGS", L".UC-UPDATE", L"RELEASE.JSON"})
        if (root == reserved) return invalid(ErrorCode::unsafe_path, "Protected user or update metadata path");
    return std::monostate{};
}
Result<Manifest> parse_manifest(std::string_view text) {
    if (text.size() > max_manifest_size) return invalid(ErrorCode::invalid_manifest, "Manifest exceeds limit");
    try {
        auto json = Json::parse(text);
        if (!json.is_object() || !json.at("format").is_number_unsigned() || json.at("format") != 1 ||
            !json.at("version").is_string() || !json.at("files").is_array() ||
            !json.at("helper_files").is_array())
            return invalid(ErrorCode::invalid_manifest, "Invalid manifest fields");
        auto version = parse_version(json.at("version").get<std::string>());
        if (failed(version)) return std::get<Error>(version);
        Manifest manifest{std::get<Version>(version), {}, {}};
        if (json.at("files").size() > max_entries)
            return invalid(ErrorCode::invalid_manifest, "Too many files");
        std::set<std::wstring> paths;
        uint64_t total = 0;
        for (const auto& entry : json.at("files")) {
            if (!entry.is_object() || !entry.at("path").is_string() || !entry.at("sha256").is_string() ||
                !unsigned_size(entry.at("size"), max_zip_size))
                return invalid(ErrorCode::invalid_manifest, "Invalid file entry");
            FileEntry file{entry.at("path").get<std::string>(), entry.at("size").get<uint64_t>(), entry.at("sha256").get<std::string>()};
            auto safe = validate_managed_path(file.path);
            if (failed(safe)) return std::get<Error>(safe);
            if (!hash_valid(file.sha256) || total > max_package_size - file.size)
                return invalid(ErrorCode::invalid_manifest, "Invalid hash or package size");
            total += file.size;
            if (!paths.insert(std::get<std::wstring>(path_key(file.path))).second)
                return invalid(ErrorCode::invalid_manifest, "Duplicate path");
            manifest.files.push_back(std::move(file));
        }
        for (const auto& path : paths)
            for (size_t slash = path.find(L'/'); slash != path.npos; slash = path.find(L'/', slash + 1))
                if (paths.contains(path.substr(0, slash)))
                    return invalid(ErrorCode::invalid_manifest, "File/directory collision");
        for (auto required : {L"UC.EXE", L"UC_LAUNCHER.EXE", L"UC_UPDATER.EXE"})
            if (!paths.contains(required)) return invalid(ErrorCode::invalid_manifest, "Missing required executable");
        std::set<std::wstring> helpers;
        for (const auto& item : json.at("helper_files")) {
            if (!item.is_string()) return invalid(ErrorCode::invalid_manifest, "Invalid helper file");
            auto path = item.get<std::string>();
            auto parsed = path_key(path);
            if (failed(parsed)) return std::get<Error>(parsed);
            auto key = std::get<std::wstring>(parsed);
            if (key.find(L'/') != key.npos || (key != L"UC_UPDATER.EXE" && !key.ends_with(L".DLL")) ||
                !paths.contains(key) || !helpers.insert(key).second)
                return invalid(ErrorCode::invalid_manifest, "Invalid helper dependency");
            manifest.helper_files.push_back(std::move(path));
        }
        if (!helpers.contains(L"UC_UPDATER.EXE"))
            return invalid(ErrorCode::invalid_manifest, "Missing updater dependency");
        return manifest;
    } catch (const Json::exception& e) {
        return invalid(ErrorCode::invalid_manifest, std::string("Malformed manifest: ") + e.what());
    }
}
Result<std::optional<Release>> select_release(std::string_view text, Version installed) {
    if (text.size() > max_metadata_size) return invalid(ErrorCode::invalid_release, "Release response exceeds limit");
    try {
        auto json = Json::parse(text);
        if (!json.is_object() || !json.at("draft").is_boolean() || !json.at("prerelease").is_boolean())
            return invalid(ErrorCode::invalid_release, "Invalid release flags");
        if (json.at("draft").get<bool>() || json.at("prerelease").get<bool>()) return std::optional<Release>{};
        auto tag = json.at("tag_name").get<std::string>();
        auto version = parse_version(tag, true);
        if (failed(version)) return std::get<Error>(version);
        if (std::get<Version>(version) <= installed) return std::optional<Release>{};
        if (!json.at("assets").is_array()) return invalid(ErrorCode::invalid_release, "Invalid release assets");
        const auto name = "UC-Windows-" + format_version(std::get<Version>(version)) + ".zip";
        const Json* asset = nullptr;
        for (const auto& item : json.at("assets")) {
            if (!item.is_object() || !item.at("name").is_string())
                return invalid(ErrorCode::invalid_release, "Invalid asset");
            if (item.at("name") != name) continue;
            if (asset) return invalid(ErrorCode::invalid_release, "Duplicate release archive");
            asset = &item;
        }
        if (!asset || asset->at("state") != "uploaded" || !unsigned_size(asset->at("size"), max_zip_size) ||
            asset->at("size") == 0)
            return invalid(ErrorCode::invalid_release, "Missing or invalid release archive");
        if (!asset->contains("digest") || !asset->at("digest").is_string())
            return invalid(ErrorCode::invalid_release, "Release SHA-256 is unavailable");
        auto digest = asset->at("digest").get<std::string>();
        if (!digest.starts_with("sha256:") || !hash_valid(std::string_view(digest).substr(7)))
            return invalid(ErrorCode::invalid_release, "Release SHA-256 is unavailable");
        auto url = asset->at("browser_download_url").get<std::string>();
        if (url != "https://github.com/solleo95/reuc-recomp/releases/download/" + tag + "/" + name)
            return invalid(ErrorCode::invalid_release, "Unexpected release download URL");
        std::string notes;
        if (json.contains("body") && !json.at("body").is_null()) notes = json.at("body").get<std::string>();
        return std::optional<Release>{Release{std::get<Version>(version), std::move(url), std::move(notes),
            digest.substr(7), asset->at("size").get<uint64_t>()}};
    } catch (const Json::exception& e) {
        return invalid(ErrorCode::invalid_release, std::string("Malformed release: ") + e.what());
    }
}
}
