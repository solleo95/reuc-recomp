#pragma once
#include <cstddef>
#include <filesystem>
#include <string>

namespace uc {
// The folder with every link on the way (junction, symbolic link, drive mounted in a folder, mapped or subst drive) resolved.
// The folder may not exist yet: its deepest existing parent is resolved and the missing names follow it.
// A path Windows cannot resolve stays as given. Result: "D:\...", "\\server\share\..." or, for a drive without a letter, "\\?\Volume{id}\...".
std::wstring resolve_folder(const std::filesystem::path& folder);
// Length of the part that is not a folder to hold: "D:\", "\\?\D:\", "\\?\Volume{id}\", "\\server\share\" and "\\?\UNC\server\share\".
size_t volume_root_length(const std::wstring& path);
}
