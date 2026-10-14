#pragma once
#include "memory.h"
#include "gx_packets.h"

namespace fifo_replay {
struct ReplayStats { uint64_t draws=0,vertices=0,display_lists=0; };
// One-shot conversion includes preceding frames so EFB and RAM dependencies survive.
class GxStream {
public:
    GxStream(const Dff& capture,size_t frame);
    std::vector<gx::FramePacket> packets();
    ReplayStats stats;
private:
    const Dff& capture_;
    Memory memory_;
    size_t target_,frame_=0;
    bool consumed_=false;
    std::array<uint32_t,256> cp_,bp_;
    std::array<uint32_t,0x1058> xf_{};
    uint32_t mask_=0xFFFFFF;
    uint64_t command_bytes_=0;
    gx::FramePacket pending_;
    std::vector<gx::FramePacket> result_;
    gx::DrawUniform colors_;
    gx::TextureCache textures_;
    struct CopiedTexture { uint32_t size;std::shared_ptr<const gx::TexturePixels> pixels; };
    std::map<uint32_t,CopiedTexture> copies_;
    std::map<uint32_t,std::pair<size_t,uint32_t>> first_copies_;
    std::vector<std::byte> tmem_;
    size_t observed_frame_=0,observed_update_=0;
    struct DeferredTexture {
        uint32_t address;gx::TextureDesc desc;gx::PaletteFormat format;
        std::vector<std::byte> palette;
        std::shared_ptr<gx::TexturePixels> target;
        uint32_t first_callsite=0,first_frame=0,cull=0;
        std::string first_state;
    };
    std::vector<DeferredTexture> deferred_;
    void walk(std::span<const std::byte> bytes,unsigned depth,uint32_t origin);
    gx::State layout() const;
    gx::Transforms transforms() const;
    void copy(uint32_t offset);
    void color_write(uint32_t reg,uint32_t value);
    void material(gx::DrawPacket& draw) const;
    void bind_textures(gx::DrawPacket& draw);
    void generate_texcoords(gx::DrawPacket& draw) const;
    void load_palette();
    void advance_memory(size_t frame,uint32_t position);
    void resolve_textures();
};
}
