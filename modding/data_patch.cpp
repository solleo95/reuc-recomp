#include "data_patch.h"
#include "mod_paths.h"
#include <algorithm>

namespace fs = std::filesystem;

namespace uc::mods {
namespace {
constexpr size_t kMaxFileBytes = 1 << 20;

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const auto dot = text.find('.', start);
        parts.push_back(text.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos) return parts;
        start = dot + 1;
    }
}

bool whole_number(const std::string& text, long long& value) {
    if (text.empty() || text.size() > 9 || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
    value = std::stoll(text);
    return true;
}
}  // namespace

PatchResult apply_data_patches(const fs::path& mod_root, const ApiRegistry& api) {
    PatchResult result;
    std::vector<fs::path> files;
    std::error_code error;
    for (fs::directory_iterator it(mod_root / "data", error), last; !error && it != last; it.increment(error)) {
        std::error_code entry_error;
        if (it->is_regular_file(entry_error) && it->path().extension() == ".json") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return a.filename().u8string() < b.filename().u8string(); });
    for (const auto& file : files) {
        const std::string name = utf8(file.filename());
        std::error_code size_error;
        if (fs::file_size(file, size_error) > kMaxFileBytes) { result.errors.push_back(name + ": file is larger than 1 MiB, skipped"); continue; }
        const auto text = read_file(file);
        if (!text) { result.errors.push_back(name + ": cannot be read"); continue; }
        const auto root = nlohmann::ordered_json::parse(*text, nullptr, false);   // ordered: the keys apply in the order the file lists them
        if (root.is_discarded()) { result.errors.push_back(name + ": not valid JSON"); continue; }
        if (!root.is_object()) { result.errors.push_back(name + ": must contain one JSON object of names to values"); continue; }
        for (const auto& [key, ordered_value] : root.items()) {
            const Value value = ordered_value;
            const auto parts = split(key);
            const auto fail = [&](const std::string& why) { result.errors.push_back(name + ": '" + key + "': " + why); };
            if (parts.size() != 2 && parts.size() != 3) { fail("expected <domain>.<member> or <domain>.<entity>.<member>"); continue; }
            const std::string path = parts.front() + "." + parts.back();
            const Member* member = api.find(path);
            if (!member) { fail("not available (unknown, or not implemented in this version: see the API reference)"); continue; }
            if (!member->set) { fail("read-only"); continue; }
            Value args = Value::object();
            if (member->scope.empty() != (parts.size() == 2)) { fail(member->scope.empty() ? "this member has no <entity> part" : "needs the " + member->scope + " in the middle"); continue; }
            if (!member->scope.empty()) {
                long long entity = 0;
                if (member->scope == "name") args[member->scope] = parts[1];
                else if (whole_number(parts[1], entity)) args[member->scope] = entity;
                else { fail("the " + member->scope + " must be a whole number, got '" + parts[1] + "'"); continue; }
            }
            try { member->set(args, value); ++result.applied; }
            catch (const std::exception& e) { fail(e.what()); }
        }
    }
    return result;
}

}  // namespace uc::mods
