#pragma once
#include "gx_vertices.h"

namespace gx {
// The game's full-screen quad helper (callsite 0x800421B8) draws every step of its post-process chain. The bloom chain blurs a half-resolution copy of
// the scene down and back up, then adds the result to the full frame: the only draw of this helper that blends source ONE with destination ONE.
// Found with the packet dump (UC_PACKET_DUMP / UC_PACKET_STATS); skipping it removes the glow around lamps and nothing else.
inline bool bloom_composite(const DrawPacket& draw) {
    return draw.callsite == 0x800421B8 && draw.blend.mode == 1 && draw.blend.source == 1 && draw.blend.destination == 1;
}
}
