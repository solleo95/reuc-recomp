#pragma once
#include "ppc_rt.h"
#include <memory>
namespace uc {
void audio_wait_ready(ppc::Ctx& c,uint32_t timeout_ms=10000);
struct AudioStartupSnapshot;
std::shared_ptr<const AudioStartupSnapshot> audio_startup_checkpoint();
void audio_startup_restore(const AudioStartupSnapshot&);
}
