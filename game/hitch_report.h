#pragma once
#include "frame_log.h"
#include <algorithm>
#include <cstdio>
#include <string>
namespace uc {
// A presented frame that took much longer than the usual one, with the biggest measured contributor named, so a user log shows why a drop happened.
// `previous` is the row before (the counters in a row are running totals). Returns an empty string when the frame is not a hitch.
inline std::string hitch_report(const FrameRow& row,const FrameRow& previous,double typical_ms) {
    if(typical_ms<=0 || row.interval_ms<std::max(typical_ms*1.8,typical_ms+8))return {};
    double guest=0;for(const double t:row.thread_ms)guest+=t;
    const double seal_wait=row.seal_wait_ns>=previous.seal_wait_ns ? (row.seal_wait_ns-previous.seal_wait_ns)/1e6 : 0;
    const struct {const char* name;double ms;} causes[]={
        {"gpu",row.gpu_sum_ms},{"guest-cpu",guest},{"guest-idle",row.idle_ms},{"present-wait",row.acquire_ms+row.present_ms},
        {"driver-submit",row.submit_ms},{"render-thread",row.busy_ms-row.acquire_ms-row.submit_ms-row.present_ms},{"window-events",row.events_ms},{"renderer-waits-guest",seal_wait}};
    const auto* top=&causes[0];for(const auto& cause:causes)if(cause.ms>top->ms)top=&cause;
    const bool known=top->ms>=row.interval_ms*0.5;
    const auto delta=[](uint64_t now,uint64_t before){return (unsigned long long)(now>=before ? now-before : 0);};
    char text[768];
    std::snprintf(text,sizeof text,"[hitch] t=%.0f ms frame=%.1f ms (typical %.1f) cause=%s | gpu %.1f gpu-longest %.1f guest-cpu %.1f guest-idle %.1f present-wait %.1f submit %.1f render %.1f events %.1f renderer-waits-guest %.1f | textures+%llu decodes+%llu disc-reads+%llu queued %u outstanding %u rt-as %.1f",
        row.t_ms,row.interval_ms,typical_ms,known ? top->name : "unattributed (OS, driver or another program)",row.gpu_sum_ms,row.gpu_ms,guest,row.idle_ms,row.acquire_ms+row.present_ms,row.submit_ms,row.busy_ms,row.events_ms,seal_wait,
        delta(row.textures,previous.textures),delta(row.decodes,previous.decodes),delta(row.dvd_reads,previous.dvd_reads),row.queued,row.outstanding,row.gpu_rt_as_ms);
    return text;
}
}
