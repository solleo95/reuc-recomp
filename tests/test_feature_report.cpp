#include "check.h"
#include "feature_report.h"
int main() {
    uc::FeatureReport report;
    report.dlss=true; report.dlss_g=true; report.dlss_g_max_multiplier=2; report.fsr=true; report.fsr_fg=true;
    report.dlss_g_reason=""; report.fsr_reason="";
    const auto back=uc::feature_report_from_json(uc::to_json(report));
    CHECK(back.dlss && back.dlss_g && back.dlss_g_max_multiplier==2 && back.fsr && back.fsr_fg);
    uc::FeatureReport none; none.dlss_reason="no NVIDIA RTX GPU"; none.dlss_g_reason="requires RTX 40 or newer"; none.fsr_reason="amd_fidelityfx_vk.dll missing"; none.fsr_fg_reason="fewer than 3 queues";
    const auto reasons=uc::feature_report_from_json(uc::to_json(none));
    CHECK(reasons.fsr_fg_reason=="fewer than 3 queues");
    CHECK(!reasons.dlss && reasons.dlss_reason=="no NVIDIA RTX GPU" && reasons.dlss_g_reason=="requires RTX 40 or newer" && reasons.fsr_reason=="amd_fidelityfx_vk.dll missing");
    {   // screen-space effects and anisotropy support
        uc::FeatureReport effects; effects.effects=true; effects.effects_reason="x"; effects.anisotropy_max=8;
        const auto round=uc::feature_report_from_json(uc::to_json(effects));
        CHECK(round.effects && round.effects_reason=="x" && round.anisotropy_max==8);
        const auto old=uc::feature_report_from_json(R"({"dlss":true})");   // a report from an older uc.exe
        CHECK(!old.effects && old.anisotropy_max==1);
    }
    {   // ray tracing and ray reconstruction support
        uc::FeatureReport rt; rt.ray_tracing=true; rt.ray_tracing_reason=""; rt.ray_reconstruction=false; rt.ray_reconstruction_reason="no DLSS-RR";
        const auto round=uc::feature_report_from_json(uc::to_json(rt));
        CHECK(round.ray_tracing && round.ray_tracing_reason.empty() && !round.ray_reconstruction && round.ray_reconstruction_reason=="no DLSS-RR");
        const auto old=uc::feature_report_from_json(R"({"dlss":true})");   // a report from an older uc.exe
        CHECK(!old.ray_tracing && !old.ray_reconstruction && old.ray_tracing_reason.empty() && old.ray_reconstruction_reason.empty());
    }
    const auto partial=uc::feature_report_from_json(R"({"dlss":true})");   // missing fields: unavailable
    CHECK(partial.dlss && !partial.dlss_g && !partial.fsr && partial.dlss_g_max_multiplier==0);
    for(auto text:{"","not json","[]",R"({"dlss":"yes"})",R"({"dlss_g_max_multiplier":-1})"}) {
        const auto bad=uc::feature_report_from_json(text);   // unreadable report: nothing is offered
        CHECK(!bad.dlss && !bad.dlss_g && !bad.fsr && !bad.fsr_fg);
    }
    return 0;
}
