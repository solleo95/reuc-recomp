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
    const auto partial=uc::feature_report_from_json(R"({"dlss":true})");   // missing fields: unavailable
    CHECK(partial.dlss && !partial.dlss_g && !partial.fsr && partial.dlss_g_max_multiplier==0);
    for(auto text:{"","not json","[]",R"({"dlss":"yes"})",R"({"dlss_g_max_multiplier":-1})"}) {
        const auto bad=uc::feature_report_from_json(text);   // unreadable report: nothing is offered
        CHECK(!bad.dlss && !bad.dlss_g && !bad.fsr && !bad.fsr_fg);
    }
    return 0;
}
