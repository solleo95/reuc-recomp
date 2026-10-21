#include "game_pacing.h"
#include <algorithm>
#include <cmath>
namespace {
constexpr uc::PacingFix kFixes[] = {
#define PACING_ENTRY(entry, frame)
#define PACING_FIX(entry, lr, tmpl, reg, power) {entry, lr, uc::PacingTemplate::tmpl, reg, power},
#include "pacing_fixes.inc"
#undef PACING_ENTRY
#undef PACING_FIX
    {0, 0, uc::PacingTemplate::scale, 0, 0}   // sentinel: keeps the array non-empty
};
constexpr size_t kCount = sizeof(kFixes) / sizeof(kFixes[0]) - 1;
void set_fpr(ppc::Ctx& c, unsigned reg, double v) { c.f[reg] = c.ps1[reg] = double(float(v)); }
// Runs every record of `entry` whose caller LR matches; at most one record applies per call.
void apply_entry(ppc::Ctx& c, uint32_t entry, uint32_t frame) {
    const auto* end = kFixes + kCount;
    const auto* fix = std::find_if(kFixes, end, [&](const uc::PacingFix& f) { return f.entry == entry && f.lr == c.lr; });
    if (fix == end) { uc::pacing_call_body(c, frame, entry + 4); return; }
    uc::pacing_apply_record(c, *fix, frame);
}
}
namespace uc {
PacingFixTable pacing_fix_table() { return {kFixes, kFixes + kCount}; }
void pacing_apply_record(ppc::Ctx& c, const PacingFix& fix, uint32_t frame) {
    const float s = game_step_scale();
    const bool active = c.lr == fix.lr && s > 0 && s != 1.f;
    const double factor = active ? std::pow(double(s), fix.power) : 1.0;
    if (active && fix.tmpl == PacingTemplate::scale) set_fpr(c, fix.reg, float(c.f[fix.reg]) * factor);
    pacing_call_body(c, frame, fix.entry + 4);
    if (!active) return;
    if (fix.tmpl == PacingTemplate::scale_ret) set_fpr(c, fix.reg, float(c.f[fix.reg]) * factor);
    if (fix.tmpl == PacingTemplate::pow_alpha) {
        const double a = std::clamp(double(c.f[fix.reg]), 0.0, 1.0);
        set_fpr(c, fix.reg, a == 1 ? 1.0 : -std::expm1(double(s) * std::log1p(-a)));
    }
}
}
#define PACING_ENTRY(entry, frame) void hle_PacingFix_##entry(ppc::Ctx& c) { apply_entry(c, entry, frame); }
#define PACING_FIX(entry, lr, tmpl, reg, power)
#include "pacing_fixes.inc"
#undef PACING_ENTRY
#undef PACING_FIX
