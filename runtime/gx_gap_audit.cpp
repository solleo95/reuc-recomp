#include "gx_gap_audit.h"
#include <algorithm>
#include <map>
#include <mutex>

namespace uc {
namespace {
std::mutex g_mutex;
bool g_enabled = false;
std::vector<uint8_t> g_stream;
bool g_stream_recording = false;
std::map<std::string, GapRow> g_rows;

uint32_t be32(size_t at) { return uint32_t(g_stream[at]) << 24 | uint32_t(g_stream[at + 1]) << 16 | uint32_t(g_stream[at + 2]) << 8 | g_stream[at + 3]; }

// Returns the command length, or 0 when more bytes are needed; fills `key`.
size_t classify(std::string& key) {
    char text[48];
    const char* where = g_stream_recording ? "list" : "live";
    const size_t have = g_stream.size();
    const uint8_t op = g_stream[0];
    size_t need = 1;
    if (op == 0x00 || op == 0x48) { key.clear(); return 1; } // NOP padding and vertex-cache invalidation carry no state
    if (op == 0x08) { need = 6; if (have < need) return 0; std::snprintf(text, sizeof text, "%s CP %02X", where, g_stream[1]); }
    else if (op == 0x10) {
        if (have < 5) return 0;
        const uint32_t header = be32(1), address = header & 0xFFFF;
        need = 5 + 4 * size_t((header >> 16) + 1); if (have < need) return 0;
        const uint32_t region = address >= 0x1000 ? address : address < 0x100 ? 0 : address < 0x400 ? 0x100 : address < 0x500 ? 0x400 : address < 0x600 ? 0x500 : 0x600;
        std::snprintf(text, sizeof text, "%s XF %04X", where, region);
    } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
        need = 5; if (have < need) return 0;
        const uint32_t address = be32(1) & 0xFFF;
        std::snprintf(text, sizeof text, "%s IDX%02X %03X", where, op, address >= 0x400 ? address & 0xF00 : 0);
    } else if (op == 0x40) { need = 9; if (have < need) return 0; std::snprintf(text, sizeof text, "%s CALLDL", where); }
    else if (op == 0x61) { need = 5; if (have < need) return 0; std::snprintf(text, sizeof text, "%s BP %02X", where, g_stream[1]); }
    else if (op & 0x80) { std::snprintf(text, sizeof text, "%s primitive %02X", where, op); need = have; } // length unknown here
    else std::snprintf(text, sizeof text, "%s unknown %02X", where, op);
    key = text;
    return need;
}
}

void gx_gap_audit_enable(bool enabled) {
    std::lock_guard lock(g_mutex);
    g_enabled = enabled; g_stream.clear(); g_rows.clear();
}
bool gx_gap_audit_enabled() { return g_enabled; }

void gx_gap_audit_write(int size, uint32_t value, bool in_primitive, bool recording, uint32_t (*attribute)()) {
    if (!g_enabled || in_primitive) return;
    std::lock_guard lock(g_mutex);
    if (g_stream.empty()) g_stream_recording = recording;
    for (int i = 0; i < size; ++i) g_stream.push_back(uint8_t(value >> ((size - 1 - i) * 8)));
    std::string key;
    while (!g_stream.empty()) {
        const size_t length = classify(key);
        if (!length) break;
        if (!key.empty()) {
            auto& row = g_rows[key];
            row.key = key; ++row.count;
            if (row.count <= 64 || row.count % 4096 == 0) {
                const uint32_t function = attribute ? attribute() : 0;
                if (function && row.functions.size() < 8 && std::find(row.functions.begin(), row.functions.end(), function) == row.functions.end())
                    row.functions.push_back(function);
            }
        }
        g_stream.erase(g_stream.begin(), g_stream.begin() + std::ptrdiff_t(length));
        g_stream_recording = recording;
    }
}

std::vector<GapRow> gx_gap_audit_rows() {
    std::lock_guard lock(g_mutex);
    std::vector<GapRow> rows;
    for (const auto& [key, row] : g_rows) rows.push_back(row);
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.count > b.count; });
    return rows;
}

void gx_gap_audit_report(std::FILE* out) {
    std::fprintf(out, "guest FIFO commands outside native HLE calls (count, command, writing guest functions)\n");
    for (const auto& row : gx_gap_audit_rows()) {
        std::fprintf(out, "%12llu  %-18s", (unsigned long long)row.count, row.key.c_str());
        for (auto function : row.functions) std::fprintf(out, " %08X", function);
        std::fprintf(out, "\n");
    }
}
}
