#include "check.h"
#include "gx_gap_audit.h"
#include <algorithm>

static uint32_t g_function = 0x80123450;
static uint32_t attribute() { return g_function; }

int main() {
    uc::gx_gap_audit_enable(true);
    auto w = [](int size, uint32_t value, bool primitive = false, bool list = false) { uc::gx_gap_audit_write(size, value, primitive, list, attribute); };
    w(1, 0x08); w(1, 0x50); w(4, 0x00000600);                 // CP 0x50 (vertex descriptor)
    w(1, 0x10); w(4, 0x00001018); w(4, 0x00000003);           // XF register 0x1018
    w(1, 0x61); w(4, 0xF219141A);                             // BP 0xF2 (fog colour)
    w(1, 0x28); w(4, 0x00058409);                             // indexed normal matrix load
    w(1, 0x40); w(4, 0x80400000); w(4, 0x00000020);           // display list call
    w(1, 0x61, true); w(4, 0x40000000, true);                 // vertex bytes inside a primitive are ignored
    g_function = 0x80999990;
    w(1, 0x61, false, true); w(4, 0xF2000000, false, true);   // the same BP register while recording a list
    const auto rows = uc::gx_gap_audit_rows();
    auto find = [&](const char* key) { return std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.key == key; }); };
    CHECK(rows.size() == 6);
    CHECK(find("live CP 50") != rows.end() && find("live CP 50")->count == 1);
    CHECK(find("live XF 1018") != rows.end());
    CHECK(find("live BP F2") != rows.end() && find("live BP F2")->functions == std::vector<uint32_t>{0x80123450});
    CHECK(find("live IDX28 400") != rows.end());
    CHECK(find("live CALLDL") != rows.end());
    CHECK(find("list BP F2") != rows.end() && find("list BP F2")->functions == std::vector<uint32_t>{0x80999990});
    for (int i = 0; i < 40; ++i) { g_function = 0x80000000 + i * 4; w(1, 0x61); w(4, 0xF1000000); }
    CHECK(find("live BP F1") == rows.end());
    const auto again = uc::gx_gap_audit_rows();
    const auto f1 = std::find_if(again.begin(), again.end(), [](const auto& row) { return row.key == "live BP F1"; });
    CHECK(f1 != again.end() && f1->count == 40 && f1->functions.size() == 8); // attribution is bounded per key
    return 0;
}
