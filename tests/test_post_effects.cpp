#include "check.h"
#include "post_effects.h"
int main() {
    gx::DrawPacket draw;
    draw.callsite = 0x800421B8; draw.blend.mode = 1; draw.blend.source = 1; draw.blend.destination = 1;
    CHECK(gx::bloom_composite(draw));
    draw.blend.destination = 4; CHECK(!gx::bloom_composite(draw));   // the blur taps of the chain blend source alpha
    draw.blend.destination = 1; draw.blend.mode = 0; CHECK(!gx::bloom_composite(draw));
    draw.blend.mode = 1; draw.callsite = 0x80393B0C; CHECK(!gx::bloom_composite(draw));   // other additive draws (sprites, flashes) stay
    return 0;
}
