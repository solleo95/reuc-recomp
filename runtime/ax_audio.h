#pragma once
#include "ppc_rt.h"
#include "sched.h"
#include <span>
#include <functional>
namespace uc {
struct AudioStats { uint64_t frames,nonzero_samples; };
using AudioSink=std::function<void(std::span<const int16_t>)>;
void mix_ax_voice(ppc::Ctx& c,uint32_t pb,std::span<int32_t,192> mix);
void audio_hle_init(Sched& sched,AudioSink sink);
void audio_hle_shutdown();
AudioStats audio_stats();
}
void hle_AIStartDMA(ppc::Ctx& c);
void hle_AXFrame(ppc::Ctx& c);
