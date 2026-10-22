#pragma once
// The game's text files (Data/message/mes_<lang>.txtres, magic "TRES"), big-endian: a header (magic, version 0x00010000, record count, table offset),
// a table of record offsets, and 20-byte records (id, voice-name pointer, line count, text pointer, 0). Lines are NUL-terminated UTF-8 strings at
// absolute file offsets, consecutive per record; the pool is not dense (strings are shared, gaps exist). Layout found in docs/superpowers/plans/2026-10-23-asset-mods-text.md.
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace uc::mods {

struct TextRecord {
    uint32_t id = 0;
    std::vector<std::string> lines;
};

// Validates everything it reads (magic, version, counts, table, records, pointers, terminators, ascending ids); nullopt with `error` set for a damaged file.
std::optional<std::vector<TextRecord>> read_txtres(std::span<const uint8_t> file, std::string& error);

using TextChanges = std::map<uint32_t, std::vector<std::string>>;   // record id -> its new lines

struct TextPatch {
    std::vector<uint8_t> file;
    std::vector<uint32_t> unknown_ids;   // changes whose id is not in the file
};

// The file with the records of `changes` repointed to their new lines, which are appended at the end of the file: nothing else moves, so no changes
// give the same bytes. nullopt with `error` set when the file is damaged or would grow past 8 MB.
std::optional<TextPatch> patch_txtres(std::span<const uint8_t> file, const TextChanges& changes, std::string& error);

}  // namespace uc::mods
