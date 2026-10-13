#include "check.h"
#include "audio_output.h"
#include <array>
#include <vector>
int main() {
    CHECK(SDL_SetEnvironmentVariable(SDL_GetEnvironment(),"SDL_AUDIODRIVER","dummy",true));
    uc::AudioOutput output(true);
    CHECK(output.stats().blocks==0 && output.stats().overruns==0);
    const std::vector<int16_t> burst(32000*2,1234);
    output.write(burst);
    const std::array<int16_t,192> block{};
    output.write(block);
    const auto stats=output.stats();
    CHECK(stats.blocks==2 && stats.overruns==1 && stats.max_queued_bytes>12800);
}
