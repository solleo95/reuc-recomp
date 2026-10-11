#include "check.h"
#include "dol.h"
#include "dol_builder.h"
#include "dol_scan.h"
#include <string>

static uc::ScanReport scan(const std::vector<uint8_t>& f) {
    auto d = uc::parse_dol(f);
    CHECK(d.ok);
    return uc::scan_text(f, d.image);
}

int main() {
    // bl forward (+8 lands on the 3rd word), blr x2
    {
        auto r = scan(make_dol({0x48000009, 0x60000000, 0x4E800020, 0x4E800020}));
        CHECK(r.unique_bl_targets == 1);
        CHECK(r.blr_count == 2);
        CHECK(r.text_bytes == 16);
    }
    // bl backward (-4 from word 1 lands on word 0); duplicates count once
    {
        auto r = scan(make_dol({0x60000000, 0x4BFFFFFD, 0x4BFFFFF9}));  // -4 and -8 -> targets word0, word0
        CHECK(r.unique_bl_targets == 1);
    }
    // review focus 3: bl target outside every text section is not a function
    {
        auto r = scan(make_dol({0x48001001, 0x4E800020}));  // bl +0x1000
        CHECK(r.unique_bl_targets == 0);
    }
    // stwu r1,-16(r1) prologue
    {
        auto r = scan(make_dol({0x9421FFF0, 0x7C0802A6, 0x4E800020}));
        CHECK(r.stwu_prologues == 1);
    }
    // zero runs: 300 zero words are suspicious; 10 are not
    {
        std::vector<uint32_t> code(300, 0);
        code.insert(code.begin(), 0x60000000);
        code.push_back(0x4E800020);
        auto r = scan(make_dol(code));
        CHECK(r.largest_zero_run_words == 300);
        CHECK(r.suspicious_zero_runs == 1);
        std::vector<uint32_t> small(10, 0);
        small.push_back(0x4E800020);
        auto s = scan(make_dol(small));
        CHECK(s.largest_zero_run_words == 10);
        CHECK(s.suspicious_zero_runs == 0);
    }
    // review focus 2: section size not a multiple of 4 -> tail ignored, no OOB read
    {
        auto f = make_dol({0x4E800020, 0x4E800020});
        put32(f, 0x90, 6);
        auto r = scan(f);
        CHECK(r.blr_count == 1);
    }
    // SDK banner strings are found in data sections
    {
        std::string banner = "<< RVL_SDK - GX \trelease build: Jul  1 2008>>";
        std::vector<uint8_t> data(banner.begin(), banner.end());
        data.push_back(0);
        auto f = make_dol({0x60000000}, data);
        auto d = uc::parse_dol(f);
        CHECK(d.ok);
        auto s = uc::find_sdk_strings(f, d.image);
        CHECK(s.size() == 1);
        CHECK(s[0].address == 0x80400000);
        CHECK(s[0].text.find("RVL_SDK") != std::string::npos);
    }
    return 0;
}
