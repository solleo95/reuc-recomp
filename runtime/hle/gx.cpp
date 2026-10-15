#include "gx.h"
#include "guest_call.h"
#include "hle/os.h"
#include "gx_audit.h"
#include "vi.h"
#include <atomic>
#include <bit>
#include <cstring>
#include <map>

#include <variant>
#include <algorithm>
#include <string>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <list>
#include <unordered_map>
#include "clock.h"

namespace {
uc::Sched* scheduler;
gx::Packets* packets;
gx::Memory memory;
gx::State state;
gx::Transforms transforms;
// Blocks shared by consecutive draws; each path keeps its own so an interleaved path does not evict the other's.
gx::Shared<gx::Transforms> live_transforms,import_transforms,mesh_transforms_block;
gx::Shared<gx::State> live_state,import_state,mesh_state;
gx::BlendState blend_state,live_blend;
gx::DrawUniform color_uniforms;
gx::Shared<gx::DrawUniform> live_uniforms,import_uniforms,mesh_uniforms_block;
gx::TevMaterial live_material;
bool profile_mesh;
uint32_t prompt_color_logs=0;
uint64_t prompt_batch=0;
std::vector<gx::PromptDraw> carried_prompts;
std::atomic<uint64_t> prompt_annotations{},prompt_present{},prompt_fallback{};
uint32_t mesh_profile_count;
uint32_t copy_profile_count;
uint32_t channel_profile_count;
uint32_t texgen_profile_count;
using TextureMatrix=std::array<std::array<float,4>,3>;
using TextureMatrices=std::array<std::optional<TextureMatrix>,126>;
using TextureSelectors=std::array<uint32_t,8>;
TextureMatrices texture_matrices;
std::array<uint32_t,16> indirect_stages{};
struct TextureSource { uint32_t address; gx::TextureDesc desc; std::optional<uint32_t> palette; uint8_t wrap=0; };
struct PaletteSource { uint32_t address, entries; gx::PaletteFormat format; };
struct PaletteData { gx::PaletteFormat format; std::vector<std::byte> bytes; };
using Palettes=std::map<uint32_t,std::shared_ptr<const PaletteData>>;
// shortcut: SDK region identity; unify aliased regions if observed (TD-038).
Palettes palettes;
using TextureBindings=std::array<std::optional<TextureSource>,8>;
TextureBindings bound_textures;
std::array<uint8_t,8> wraps(const TextureBindings& bindings) {
    std::array<uint8_t,8> result{};
    for(size_t i=0;i<8;++i) if(bindings[i]) result[i]=bindings[i]->wrap;
    return result;
}
std::array<uint8_t,8> live_texture_wrap{};
std::shared_ptr<const gx::TexturePixels> live_texture;
std::array<std::shared_ptr<const gx::TexturePixels>,7> live_texture_maps;
struct CopyState { std::array<uint8_t,4> color{0,0,0,255}; uint32_t depth=0xFFFFFF; };
CopyState copy_state;
std::array<uint32_t,4> texture_copy_source{};
uint32_t texture_copy_width=0,texture_copy_height=0;
uint32_t texture_copy_format=6;
using CopiedTextures=std::map<uint32_t,std::shared_ptr<const gx::TexturePixels>>;
CopiedTextures copied_textures;
struct CopiedTexture { gx::TextureCopy request; bool clear; CopyState clear_state; };
using Imported=std::deque<std::variant<gx::DrawPacket,CopiedTexture>>;
std::unique_ptr<gx::Vertices> capture;
struct MeshGeometry { std::shared_ptr<gx::MeshBuffers> buffers; };   // mutable only while the cache entry is built
struct MeshPosition { uint16_t index; uint32_t row; };
struct MeshNormal { uint16_t index; uint32_t row; };
struct MeshTexture { uint16_t index; uint32_t id; uint32_t words; };
struct CachedMesh {
    struct Source { uint32_t address; std::vector<std::byte> bytes; };
    gx::State layout;
    std::vector<std::byte> bytes;
    std::vector<std::variant<MeshGeometry,MeshPosition,MeshNormal,MeshTexture>> operations;
    std::vector<Source> sources;
    size_t size=0;
    mutable uint32_t stamp=0,skips=0;   // write-barrier stamp of the last byte comparison (gx_write_watch.h); hits since then
};
struct MeshCacheEntry { uintptr_t key; std::shared_ptr<const CachedMesh> mesh; };
std::list<MeshCacheEntry> mesh_cache;
std::unordered_map<uintptr_t,std::list<MeshCacheEntry>::iterator> mesh_index;
size_t mesh_cache_bytes=0;
constexpr size_t mesh_cache_budget=32u<<20;
std::atomic<uint64_t> geometry_hits{0},geometry_misses{0};
std::deque<gx::DrawPacket,gx::PoolAllocator<gx::DrawPacket>> draws;
struct RecordedDraw {
    gx::State state;
    uint32_t callsite;
    uint8_t primitive, vat;
    uint16_t count;
    size_t expected;
    std::vector<std::byte> payload;
    gx::TevMaterial material;
    gx::DrawUniform uniforms;
    TextureSelectors texture_selectors;
    std::optional<uc::PromptDescription> prompt;
    bool compact=false;
};
struct Resource {
    std::vector<std::byte> bytes;
    mutable uint32_t stamp=0,skips=0;   // write-barrier stamp of the last byte comparison (gx_write_watch.h); uses since then
    struct ArrayBinding { size_t attribute; gx::Array array; };
    struct Child { uint32_t address, length; };
    struct Projection { gx::Projection matrix; };
    struct Position { uint32_t row; gx::PositionMatrix matrix; };
    struct IndexedPosition { uint16_t index; uint32_t row; };
    struct IndexedNormal { uint16_t index; uint32_t row; };
    struct BpRegister { uint32_t reg, value; };
    struct Current { uint32_t row; };
    struct CopyClear { CopyState state; };
    struct TextureCopy { std::array<uint32_t,4> source; uint32_t address,width,height; bool clear; uint32_t format=6; };
    struct TextureBinding { uint32_t map; TextureSource source; };
    struct PaletteLoad { uint32_t region; PaletteSource source; };
    struct Blend { gx::BlendState state; };
    struct TevColor { uint32_t id; bool konst; std::array<float,4> value; };
    struct Light { uint32_t id; gx::LightUniform value; };
    struct Normal { uint32_t row; gx::NormalMatrix value; };
    struct NumChannels { uint32_t count; };
    struct ChannelControl { uint32_t index,control; };
    struct TextureTransform { uint32_t id; TextureMatrix matrix; };
    // A RecordedDraw is ~5 KB: held by pointer so growing the vector moves 8 bytes, not the draw.
    using Item=std::variant<std::unique_ptr<RecordedDraw>,ArrayBinding,Child,Projection,Position,IndexedPosition,IndexedNormal,BpRegister,Current,CopyClear,TextureCopy,TextureBinding,PaletteLoad,Blend,TevColor,Light,Normal,NumChannels,ChannelControl,TextureTransform>;
    std::vector<Item> geometry;
};
RecordedDraw* recorded_draw(Resource::Item& item) { auto* held=std::get_if<std::unique_ptr<RecordedDraw>>(&item); return held ? held->get() : nullptr; }
const RecordedDraw* recorded_draw(const Resource::Item& item) { auto* held=std::get_if<std::unique_ptr<RecordedDraw>>(&item); return held ? held->get() : nullptr; }
std::map<uint32_t,Resource> resources; // owned SDK recordings; no FIFO command decoding
struct ListRecording {
    uint32_t address, capacity, sdk, owner;
    bool restore, overflow = false;
    std::array<std::byte,0x600> shadow{};
    gx::DrawUniform color_shadow;
    std::array<uint32_t,4> texture_copy_source_shadow;
    uint32_t texture_copy_width_shadow=0,texture_copy_height_shadow=0;
    uint32_t texture_copy_format_shadow=6;
    TextureMatrices texture_shadow;
    std::vector<std::byte> bytes;
    Resource native;
    gx::State input_state;
};
std::unique_ptr<ListRecording> recording;
std::map<uint64_t, uint32_t> finishes; // serial -> submitting thread's SDA base; guest baton only
std::atomic<uint64_t> draw_count{0}, vertex_count{0}, submitted{0}, completed{0}, outside_bytes{0};
std::atomic<uint64_t> copies{0};
std::atomic<uint64_t> texture_decodes{0};
uint32_t read32(uint32_t address) {
    auto bytes = memory.read(address, 4);
    uint32_t value = 0;
    for (auto byte : bytes) value = value << 8 | std::to_integer<uint8_t>(byte);
    return value;
}
TextureSelectors texture_selectors(uint32_t sdk) {
    TextureSelectors result{};
    const auto low=read32(sdk+0x80),high=read32(sdk+0x84);
    for(uint32_t i=0;i<8;++i) result[i]=((i<4 ? low : high)>>(i<4 ? 6+i*6 : (i-4)*6))&63;
    return result;
}
// Texture matrix ids follow GX: 0..61 (GX_IDENTITY 60), post matrices at 64+row (XF 0x500+row*4).
void resolve_texture_matrices(gx::DrawUniform& uniforms,const TextureSelectors& selectors,const TextureMatrices& matrices) {
    constexpr TextureMatrix identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};
    for(size_t i=0;i<8;++i) {
        const auto& rows=selectors[i]!=60 && matrices[selectors[i]] ? *matrices[selectors[i]] : identity;
        const auto post=64+(uniforms.tex_posts[i]&63);
        const auto& post_rows=post<matrices.size() && matrices[post] ? *matrices[post] : identity;
        for(size_t row=0;row<3;++row) { uniforms.tex_rows[i*3+row]=rows[row]; uniforms.post_rows[i*3+row]=post_rows[row]; }
    }
}
std::pair<gx::TevMaterial,gx::DrawUniform> material_snapshot(ppc::Ctx& c) {
    const auto sdk=read32(c.r[2]-8968);
    memory.read(sdk,0x600);
    const auto selectors=texture_selectors(sdk);
    gx::TevMaterial material; material.stages=((read32(sdk+0x254)>>10)&15)+1;
    auto uniforms=color_uniforms;
    uniforms.pixel[1]=read32(sdk+0x228)&31;
    uniforms.pixel[2]=read32(sdk+0x224)&511;
    uniforms.pixel[3]=(read32(sdk+0x254)>>4)&3;
    for(uint32_t i=0;i<2;++i) for(uint32_t lane=0;lane<4;++lane) {
        uniforms.materials[i][lane]=((read32(sdk+0xB0+i*4)>>(24-lane*8))&255)/255.f;
        uniforms.ambient[i][lane]=((read32(sdk+0xA8+i*4)>>(24-lane*8))&255)/255.f;
    }
    for(uint32_t i=0;i<4;++i) uniforms.channels[i]=read32(sdk+0xB8+i*4);
    for(size_t i=0;i<4;++i) material.swaps[i]=uint8_t((read32(sdk+0x200+i*8)&15)|((read32(sdk+0x204+i*8)&15)<<4));
    for(uint32_t i=0;i<material.stages;++i) {
        auto& stage=material.stage[i];
        stage.color=read32(sdk+0x180+i*4)&0xFFFFFF; stage.alpha=read32(sdk+0x1C0+i*4)&0xFFFFFF;
        stage.order=(read32(sdk+0x150+i/2*4)>>((i%2)*12))&0xFFF;
        stage.konst=(read32(sdk+0x200+i/2*4)>>(4+(i%2)*10))&1023;
        stage.indirect=indirect_stages[i];
        uniforms.ind_cmd[i]=stage.indirect;
    }
    for(uint32_t i=0;i<8;++i) {
        const auto generator=read32(sdk+0xC8+i*4),source=(generator>>7)&31;
        uniforms.tex_gens[i]=generator; uniforms.tex_posts[i]=read32(sdk+0xE8+i*4); // dual-texture post is always on (XF 0x1012 set once by __GXInitRevisionBits)
        static int unsupported_logs=0;
        if(((generator>>4)&7 || source==2 || source==3 || source==4 || source>12) && unsupported_logs<8 && ++unsupported_logs)
            std::fprintf(stderr,"[native-texgen] unsupported generator %08X slot %u callsite %08X\n",generator,i,c.lr);
        const auto id=selectors[i];
        if(profile_mesh && texgen_profile_count<16 && (source<5 || (generator&2))) {
            const bool used=std::any_of(material.stage.begin(),material.stage.begin()+material.stages,[&](const auto& stage){return (stage.order&64) && ((stage.order>>3)&7)==i;});
            if(used) { ++texgen_profile_count; std::fprintf(stderr,"[native-texgen] callsite %08X slot %u generator %08X matrix %u post %08X\n",c.lr,i,generator,id,read32(sdk+0xE8+i*4)); }
        }
    }
    resolve_texture_matrices(uniforms,selectors,texture_matrices);
    return {material,uniforms};
}
uint32_t texture_mask(const gx::TevMaterial& material,const gx::DrawUniform& uniforms) {
    uint32_t mask=1;
    for(uint32_t i=0;i<material.stages;++i) {
        const auto& stage=material.stage[i];
        if(!(stage.order&64)) continue;
        mask|=1u<<(stage.order&7);
        if(uniforms.ind_cmd[i]) mask|=1u<<((uniforms.indirect[9]>>((uniforms.ind_cmd[i]&3)*6))&7);
    }
    return mask;
}
uint32_t producer() { return scheduler->in_irq() ? UINT32_MAX : uint32_t(scheduler->current()->id); }
gx::PositionMatrix position_matrix(uint32_t address) {
    memory.read(address,48);
    gx::PositionMatrix result{};
    for (size_t r=0;r<3;++r) for (size_t col=0;col<4;++col) result[r][col]=std::bit_cast<float>(read32(address+uint32_t(r*16+col*4)));
    return result;
}
void indexed_position(gx::Transforms& target,const gx::State& bindings,uint16_t index,uint32_t row) {
    const auto& array=bindings.arrays[12];
    const uint64_t address=uint64_t(array.address)+uint64_t(index)*array.stride;
    if (!array.address || !array.stride || address>UINT32_MAX) throw gx::Error(gx::ErrorCode::array_bounds,array.address,48,array.stride);
    gx::set_position_matrix(target,row,position_matrix(uint32_t(address)));
}
// GX_NRM_MTX_ARRAY entry: nine consecutive floats, as the hardware's indexed XF load reads them.
void indexed_normal(gx::Transforms& target,const gx::State& bindings,uint16_t index,uint32_t row) {
    const auto& array=bindings.arrays[13];
    const uint64_t source=uint64_t(array.address)+uint64_t(index)*array.stride;
    if(!array.address || !array.stride || source>UINT32_MAX) throw gx::Error(gx::ErrorCode::array_bounds,array.address,36,array.stride);
    memory.read(uint32_t(source),36);
    gx::NormalMatrix matrix{};
    for(uint32_t i=0;i<9;++i) matrix[i/3][i%3]=std::bit_cast<float>(read32(uint32_t(source)+i*4));
    gx::set_normal_matrix(target,row,matrix);
}
// GX_TEXMTX_ARRAY entry (arrays[14]): a 3x4 or 2x4 matrix of consecutive floats, as the hardware's indexed XF load reads it.
void indexed_texture(TextureMatrices& target,const gx::State& bindings,uint16_t index,uint32_t id,uint32_t words) {
    const auto& array=bindings.arrays[14];
    const uint64_t source=uint64_t(array.address)+uint64_t(index)*array.stride;
    if(!array.address || !array.stride || source>UINT32_MAX) throw gx::Error(gx::ErrorCode::array_bounds,array.address,words*4,array.stride);
    memory.read(uint32_t(source),words*4);
    TextureMatrix matrix{{{},{},{0,0,1,0}}};
    for(uint32_t i=0;i<words;++i) { const auto value=std::bit_cast<float>(read32(uint32_t(source)+i*4)); if(!std::isfinite(value)) throw gx::Error(gx::ErrorCode::layout,uint32_t(source),0,i); matrix[i/4][i%4]=value; }
    target[id]=matrix;
}
bool apply_bp(gx::DrawUniform& target,uint32_t reg,uint32_t data) {
    if(reg>=0xEE && reg<=0xF2) target.fog[reg-0xEE]=data;
    else if(reg>=0x06 && reg<=0x0E) target.indirect[reg-0x06]=data;
    else if(reg==0x27) target.indirect[9]=data;
    else if(reg==0x25 || reg==0x26) target.indirect[10+reg-0x25]=data;
    else if(reg>=0x10 && reg<=0x1F) target.ind_cmd[reg-0x10]=data;
    else return false;
    return true;
}
// BP state set by a replaced SDK call: emit the word the SDK writes, then apply it natively (into a recording too, as the SDK does).
void native_bp(uint32_t word) {
    uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,word);
    const uint32_t reg=word>>24,data=word&0xFFFFFF;
    if(apply_bp(color_uniforms,reg,data)) {
        if(reg>=0x10 && reg<=0x1F) indirect_stages[reg-0x10]=data;
        if(recording && !recording->overflow) recording->native.geometry.push_back(Resource::BpRegister{reg,data});
    }
}
bool recording_primitive() {
    if (!recording || recording->native.geometry.empty()) return false;
    auto* draw=recorded_draw(recording->native.geometry.back());
    return draw && draw->payload.size()<draw->expected;
}
gx::State snapshot(ppc::Ctx& ctx) {
    uint32_t sdk = read32(ctx.r[2] - 8968);
    uint32_t low = read32(sdk + 0x14), high = read32(sdk + 0x18);
    gx::State result = recording ? recording->input_state : state;
    result.cull=uint8_t((read32(sdk+0x254)>>14)&3);
    std::array<float,6> viewport{};
    for(uint32_t i=0;i<6;++i) viewport[i]=std::bit_cast<float>(read32(sdk+0x544+i*4));
    if(viewport[2]!=0 || viewport[3]!=0) {
        for(auto value:viewport) if(!std::isfinite(value)) throw std::invalid_argument("nonfinite native viewport");
        if(viewport[2]<=0 || viewport[3]<=0 || viewport[4]<0 || viewport[4]>1 || viewport[5]<0 || viewport[5]>1) throw std::invalid_argument("invalid native viewport extent/depth");
        result.viewport=viewport;
    }
    const auto top=read32(sdk+0x148),bottom=read32(sdk+0x14C);
    if((top>>24)==0x20 && (bottom>>24)==0x21) {
        const auto left=int32_t((top>>12)&2047)-342,upper=int32_t(top&2047)-342;
        const auto right=int32_t((bottom>>12)&2047)-342,lower=int32_t(bottom&2047)-342;
        if(right<left || lower<upper) throw std::invalid_argument("invalid native scissor bounds");
        result.scissor=std::array<int32_t,4>{left,upper,right-left+1,lower-upper+1};
    }
    for (size_t i = 0; i < 9; ++i) result.descriptors[i] = low & (1u << i) ? gx::Mode::direct : gx::Mode::none;
    for (size_t i = 0; i < 4; ++i) result.descriptors[9 + i] = gx::Mode((low >> (9 + i * 2)) & 3);
    for (size_t i = 0; i < 8; ++i) result.descriptors[13 + i] = gx::Mode((high >> (i * 2)) & 3);
    for (size_t vat = 0; vat < 8; ++vat) {
        uint32_t a = read32(sdk + 0x1C + uint32_t(vat * 4));
        uint32_t b = read32(sdk + 0x3C + uint32_t(vat * 4));
        uint32_t c = read32(sdk + 0x5C + uint32_t(vat * 4));
        auto& formats = result.formats[vat];
        formats[9] = {gx::Type((a >> 1) & 7), uint8_t(2 + (a & 1)), uint8_t((a >> 4) & 31)};
        formats[10] = {gx::Type((a >> 10) & 7), uint8_t((a & 512) ? 9 : 3), 0, gx::Color::rgba8, bool(a >> 31)};
        formats[11].color = gx::Color((a >> 14) & 7); formats[12].color = gx::Color((a >> 18) & 7);
        auto texture = [&](size_t attr, uint32_t word, unsigned bit, uint32_t fraction) {
            formats[attr] = {gx::Type((word >> (bit + 1)) & 7), uint8_t(1 + ((word >> bit) & 1)), uint8_t(fraction & 31)};
        };
        texture(13, a, 21, a >> 25); texture(14, b, 0, b >> 4); texture(15, b, 9, b >> 13);
        texture(16, b, 18, b >> 22); texture(17, b, 27, c); texture(18, c, 5, c >> 9);
        texture(19, c, 14, c >> 18); texture(20, c, 23, c >> 27);
    }
    return result;
}
const Resource& checked_resource(uint32_t address,uint32_t length) {
    const auto bytes=memory.read(address,length);
    auto found=resources.find(address & 0x3FFFFFFF);
    // Compared against guest memory only when a store touched the pages since the last comparison, and anyway every 256th use.
    const bool known=found!=resources.end() && found->second.bytes.size()==length;
    const bool clean=known && found->second.skips<256 && !gx::written_since(address,length,found->second.stamp);
    if (known && !clean && gx::same_bytes(bytes,found->second.bytes)) { found->second.stamp=gx::watch_stamp(); found->second.skips=0; }
    else if (clean) ++found->second.skips;
    else {
        std::fprintf(stderr,"[native-resource-missing] address %08X length %u prefix",address,length);
        for (auto byte:bytes.first(std::min(bytes.size(),size_t(256)))) std::fprintf(stderr," %02X",std::to_integer<unsigned>(byte));
        std::fprintf(stderr,"\n");
        throw gx::Error(gx::ErrorCode::layout,address,length,0); // TD-014: prebuilt/modified lists require native import
    }
    return found->second;
}
std::shared_ptr<const PaletteData> load_palette(const PaletteSource& source) {
    if (!source.entries || source.entries>16384 || uint32_t(source.format)>2)
        throw gx::TextureError(gx::TextureErrorCode::descriptor,"invalid native palette descriptor");
    auto bytes=memory.read(source.address,source.entries*2);
    return std::make_shared<const PaletteData>(PaletteData{source.format,{bytes.begin(),bytes.end()}});
}
gx::TextureCache texture_cache;
std::shared_ptr<const gx::TexturePixels> make_copy_texture(uint32_t address,uint32_t width,uint32_t height,uint32_t format) {
    memory.read(address,gx::encoded_texture_size({width,height,format==0x27 ? gx::TextureFormat::i8 : gx::TextureFormat(format),1}));
    auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true;
    target->channels=format==0x27 ? gx::TextureChannels::alpha_intensity : format==4 ? gx::TextureChannels::rgb565 : gx::TextureChannels::rgba;
    for(;;) {
        target->levels.push_back({width,height,{}});
        if(width==1 && height==1) break;
        width=std::max(1u,width/2); height=std::max(1u,height/2);
    }
    return target;
}
std::shared_ptr<const gx::TexturePixels> snapshot_texture(const std::optional<TextureSource>& source,const Palettes& loaded,const CopiedTextures& versions=copied_textures) {
    if (!source) return {};
    if(auto found=versions.find(source->address&0x3FFFFFFF);found!=versions.end() && found->second) {
        const auto& level=found->second->levels[0];
        const auto expected=found->second->channels==gx::TextureChannels::alpha_intensity ? gx::TextureFormat::i8 : found->second->channels==gx::TextureChannels::rgb565 ? gx::TextureFormat::rgb565 : gx::TextureFormat::rgba8;
        if(source->desc.format!=expected || source->palette || source->desc.width>level.width || source->desc.height>level.height) throw std::invalid_argument("unsupported native EFB texture view");
        // shortcut: cropped GX views share the GPU copy; exact sampler/view coordinates remain TD-045.
        return found->second;
    }
    const auto bytes=memory.read(source->address,gx::encoded_texture_size(source->desc));
    gx::PaletteView palette;
    if (source->palette) {
        auto found=loaded.find(*source->palette);
        if (found==loaded.end()) throw gx::TextureError(gx::TextureErrorCode::descriptor,"GX texture uses an unloaded palette");
        palette={found->second->format,found->second->bytes};
    }
    auto result=texture_cache.snapshot(source->address,source->desc,bytes,palette);
    texture_decodes=texture_cache.decodes();
    return result;
}
void import_resource(uint32_t address,uint32_t length,gx::State& bindings,gx::Transforms& transform_state,CopyState& copy,TextureBindings& textures,Palettes& loaded,gx::BlendState& blend,gx::DrawUniform& colors,TextureMatrices& matrices,Imported& imported,CopiedTextures& versions,std::vector<uint32_t>& path) {
    const uint32_t physical=address & 0x3FFFFFFF;
    // shortcut: nesting capped at64; replace traversal if a verified deeper native asset appears (TD-032).
    if (path.size()==64 || std::find(path.begin(),path.end(),physical)!=path.end())
        throw gx::Error(gx::ErrorCode::sequence,address,64,path.size());
    const auto& resource=checked_resource(address,length);
    path.push_back(physical);
    for (const auto& item : resource.geometry) {
        if (const auto* source=recorded_draw(item)) {
            // shortcut: decode attributes per call; cache native mesh revisions after profiling (TD-032).
            auto layout=source->state;
            layout.arrays=bindings.arrays;
            gx::Vertices vertices(memory);
            vertices.begin(gx::share(import_state,layout),source->primitive,source->vat,source->count,producer(),source->callsite);
            vertices.write_attribute(producer(),source->payload);
            auto draw=vertices.take_draw(); draw.transforms=gx::share(import_transforms,transform_state);
            draw.texture=snapshot_texture(textures[0],loaded,versions);
            const auto maps=texture_mask(source->material,source->uniforms);
            for(uint32_t map=1;map<8;++map) if(maps&(1u<<map)) {
                draw.texture_maps[map-1]=snapshot_texture(textures[map],loaded,versions);
            }
            draw.material=source->material;
            auto uniforms=source->uniforms;
            uniforms.registers=colors.registers; uniforms.konst=colors.konst;
            uniforms.lights=colors.lights; uniforms.materials=colors.materials;
            uniforms.ambient=colors.ambient; uniforms.channels=colors.channels;
            uniforms.pixel[3]=colors.pixel[3];
            resolve_texture_matrices(uniforms,source->texture_selectors,matrices);
            draw.uniforms=gx::share(import_uniforms,uniforms);
            draw.texture_wrap=wraps(textures);
            draw.blend=blend;
            if(source->prompt)if(auto prompt=uc::prompt_from_quad(draw,*source->prompt)) {
                if(profile_mesh && source->compact && prompt_color_logs++<12)std::fprintf(stderr,"[prompt-color] action %u pos %.2f %.2f rgba %u %u %u %u material %.2f %.2f %.2f channel %08X\n",unsigned(source->prompt->action),prompt->efb_corners[0],prompt->efb_corners[1],draw.vertices[0].color[0][0],draw.vertices[0].color[0][1],draw.vertices[0].color[0][2],draw.vertices[0].color[0][3],draw.uniforms->materials[0][0],draw.uniforms->materials[0][1],draw.uniforms->materials[0][2],draw.uniforms->channels[0]);
                prompt->compact=source->compact;draw.native_prompt=std::move(*prompt);++prompt_annotations;
            }
            imported.push_back(std::move(draw));
        } else if (const auto* array=std::get_if<Resource::ArrayBinding>(&item)) bindings.arrays[array->attribute]=array->array;
        else if (const auto* projection=std::get_if<Resource::Projection>(&item)) transform_state.projection=projection->matrix;
        else if (const auto* matrix=std::get_if<Resource::Position>(&item)) gx::set_position_matrix(transform_state,matrix->row,matrix->matrix);
        else if (const auto* matrix=std::get_if<Resource::IndexedPosition>(&item)) indexed_position(transform_state,bindings,matrix->index,matrix->row);
        else if (const auto* matrix=std::get_if<Resource::IndexedNormal>(&item)) indexed_normal(transform_state,bindings,matrix->index,matrix->row);
        else if (const auto* bp=std::get_if<Resource::BpRegister>(&item)) apply_bp(colors,bp->reg,bp->value);
        else if (const auto* current=std::get_if<Resource::Current>(&item)) transform_state.current=current->row;
        else if (const auto* clear=std::get_if<Resource::CopyClear>(&item)) copy=clear->state;
        else if (const auto* command=std::get_if<Resource::TextureCopy>(&item)) {
            auto target=make_copy_texture(command->address,command->width,command->height,command->format);
            versions[command->address&0x3FFFFFFF]=target;
            imported.push_back(CopiedTexture{{command->source,std::move(target)},command->clear,copy});
        }
        else if (const auto* binding=std::get_if<Resource::TextureBinding>(&item)) textures[binding->map]=binding->source;
        else if (const auto* palette=std::get_if<Resource::PaletteLoad>(&item)) loaded[palette->region]=load_palette(palette->source);
        else if (const auto* value=std::get_if<Resource::Blend>(&item)) blend=value->state;
        else if (const auto* value=std::get_if<Resource::TevColor>(&item)) {
            if(value->konst) colors.konst[value->id]=value->value;
            else colors.registers[value->id]=value->value;
        }
        else if (const auto* value=std::get_if<Resource::Light>(&item)) colors.lights[value->id]=value->value;
        else if (const auto* value=std::get_if<Resource::Normal>(&item)) gx::set_normal_matrix(transform_state,value->row,value->value);
        else if (const auto* value=std::get_if<Resource::NumChannels>(&item)) colors.pixel[3]=value->count;
        else if (const auto* value=std::get_if<Resource::ChannelControl>(&item)) colors.channels[value->index]=value->control;
        else if (const auto* value=std::get_if<Resource::TextureTransform>(&item)) matrices[value->id]=value->matrix;
        else {
            const auto& child=std::get<Resource::Child>(item);
            import_resource(child.address,child.length,bindings,transform_state,copy,textures,loaded,blend,colors,matrices,imported,versions,path);
        }
    }
    path.pop_back();
}
uint64_t submit_batch(bool present,bool clear,uint32_t xfb=0,std::optional<gx::TextureCopy> texture_copy={},std::array<uint32_t,4> display_source={0,0,640,480},bool black=false) {
    if (!packets) return 0;
    try {
        gx::FramePacket frame{0,std::move(draws),present,clear,copy_state.color,copy_state.depth,xfb,std::move(texture_copy),display_source};
        frame.black_output=black;
        if(frame.texture_copy && clear) {
            // The copy erases the EFB: prompts drawn so far leave with it, originals stay in the copied texture.
            carried_prompts.clear();
            for(const auto& draw:frame.draws)if(draw.native_prompt)++prompt_fallback;
        } else {
            // EFB content persists across DrawDone/non-clearing copies until the display copy presents it.
            for(auto& draw:frame.draws)if(draw.native_prompt)carried_prompts.push_back(std::move(*draw.native_prompt));
            std::erase_if(frame.draws,[](const auto& draw){return draw.native_prompt.has_value();});
            if(present) {prompt_present+=carried_prompts.size();frame.prompts=std::move(carried_prompts);carried_prompts.clear();}
        }
        ++prompt_batch;
        const uint64_t serial=packets->seal_batch(std::move(frame));
        draws.clear(); return serial;
    } catch (const gx::Error& error) {
        if (error.code==gx::ErrorCode::canceled) throw uc::ThreadExit{};
        throw;
    }
}
void request_finish(ppc::Ctx& ctx) {
    if (recording) throw gx::Error(gx::ErrorCode::sequence,0x802848AC,0,1);
    capture->finish();
    auto& sched = *scheduler;
    bool interrupts = sched.set_interrupts(false);
    memory.read(ctx.r[13] - 25328, 1);
    ppc::wr8(ctx, ctx.r[13] - 25328, 0);
    if (packets) {
        const uint64_t serial=submit_batch(false,false);
        finishes.emplace(serial,ctx.r[13]); ++submitted;
    }
    // Headless is runtime-only diagnostics: it submits no work and cannot signal a graphics fence.
    sched.set_interrupts(interrupts);
}
}
namespace uc {
bool gx_fifo_in_primitive() { return (capture && capture->active()) || recording_primitive(); }
bool gx_fifo_recording() { return recording != nullptr; }
GxPromptCheckpoint gx_prompt_checkpoint() {
    if(recording)return {prompt_batch,draws.size(),producer(),recording->address,recording->native.geometry.size()};
    return {prompt_batch,draws.size(),producer(),0,0};
}
bool gx_prompt_attach(GxPromptCheckpoint checkpoint,const PromptDescription& description,bool compact) {
    if(checkpoint.batch!=prompt_batch || checkpoint.producer!=producer())return false;
    if(recording) {
        auto& geometry=recording->native.geometry;
        if(recording->overflow || checkpoint.list!=recording->address || geometry.size()<=checkpoint.recorded)return false;
        RecordedDraw* quad=nullptr;
        for(size_t i=checkpoint.recorded;i<geometry.size();++i)if(auto* draw=recorded_draw(geometry[i])) {
            if(quad)return false; // More than one primitive: not a single glyph/picture quad.
            quad=draw;
        }
        if(!quad || quad!=recorded_draw(geometry.back()) || quad->primitive!=0x80 || quad->count!=4)return false;
        quad->prompt=description;quad->compact=compact;return true;
    }
    if(checkpoint.list || draws.size()!=checkpoint.draw+1)return false;
    auto prompt=prompt_from_quad(draws.back(),description);
    if(!prompt)return false;
    prompt->compact=compact;draws.back().native_prompt=std::move(*prompt);++prompt_annotations;return true;
}
void gx_hle_init(Sched& sched, uint8_t* base, gx::Packets* queue, bool profile) {
    prompt_batch=0;carried_prompts.clear();
    prompt_annotations=prompt_present=prompt_fallback=0;
    profile_mesh=profile; mesh_profile_count=0;
    prompt_color_logs=0;
    copy_profile_count=0;
    channel_profile_count=0;
    texgen_profile_count=0;
    scheduler = &sched; packets = queue;
    memory = {{reinterpret_cast<const std::byte*>(base), 24u << 20}, {reinterpret_cast<const std::byte*>(base + 0x10000000), 64u << 20}, {reinterpret_cast<const std::byte*>(base + 0x20000000), 0x4000}};
    state = {}; transforms={}; live_transforms=gx::Transforms{}; import_transforms=gx::Transforms{}; mesh_transforms_block=gx::Transforms{}; live_state=gx::State{}; import_state=gx::State{}; mesh_state=gx::State{}; import_uniforms=gx::DrawUniform{}; mesh_uniforms_block=gx::DrawUniform{}; draws.clear(); finishes.clear(); resources.clear(); recording.reset(); capture = std::make_unique<gx::Vertices>(memory);
    copy_state={}; copies=0;
    texture_copy_source={}; texture_copy_width=texture_copy_height=0; texture_copy_format=6; copied_textures.clear();
    texture_cache.clear(); texture_decodes=0;
    mesh_index.clear(); mesh_cache.clear(); mesh_cache_bytes=0; geometry_hits=0; geometry_misses=0;
    blend_state={}; live_blend={}; color_uniforms={}; live_uniforms=gx::DrawUniform{}; live_material={}; texture_matrices={}; indirect_stages={};
    bound_textures={}; palettes.clear(); live_texture.reset(); live_texture_maps={};
    draw_count = 0; vertex_count = 0; submitted = 0; completed = 0; outside_bytes = 0;
}
void gx_fifo_write(int size, uint32_t value) {
    if (size != 1 && size != 2 && size != 4) throw std::invalid_argument("invalid native GX write width");
    if (recording) {
        if (producer() != recording->owner) throw gx::Error(gx::ErrorCode::producer,recording->address,recording->owner,producer());
        if (recording->overflow) return; // GXEndDisplayList reports this failed recording with length zero
        if (size_t(size) > recording->capacity - recording->bytes.size()) { recording->overflow=true; return; }
        if (recording_primitive()) {
            auto& draw=*recorded_draw(recording->native.geometry.back());
            if (size_t(size)>draw.expected-draw.payload.size()) throw gx::Error(gx::ErrorCode::truncated,draw.callsite,draw.expected-draw.payload.size(),size);
            for (int i=0;i<size;++i) draw.payload.push_back(std::byte(uint8_t(value>>((size-1-i)*8))));
        }
        for (int i = 0; i < size; ++i) recording->bytes.push_back(std::byte(uint8_t(value >> ((size - 1 - i) * 8))));
        return;
    }
    if (!capture || !capture->active()) { outside_bytes += uint64_t(size); return; } // existing TD-014; all state bytes need owners before game-picture acceptance
    std::byte bytes[4];
    for (int i = 0; i < size; ++i) bytes[i] = std::byte(uint8_t(value >> ((size - 1 - i) * 8)));
    capture->write_attribute(producer(), {bytes, size_t(size)});
    if (!capture->active()) {
        auto draw = capture->take_draw(); draw.transforms=live_transforms; vertex_count += draw.vertices.size(); ++draw_count;
        draw.texture=live_texture;
        draw.material=live_material; draw.uniforms=live_uniforms;
        draw.texture_maps=live_texture_maps;
        draw.texture_wrap=live_texture_wrap;
        draw.blend=live_blend;
#ifdef UC_TRACE
        static uint32_t movie_draw_logs=0;
        if (draw.callsite==0x801619C4 && draw.texture && movie_draw_logs++<8) {
            std::array<uint8_t,4> low{255,255,255,255},high{};
            const auto& rgba=draw.texture->levels[0].rgba;
            for (size_t i=0;i<rgba.size();++i) { low[i%4]=std::min(low[i%4],rgba[i]); high[i%4]=std::max(high[i%4],rgba[i]); }
            std::fprintf(stderr,"[movie-draw] range %u:%u %u:%u %u:%u %u:%u blend %u %u %u mask %u\n",low[0],high[0],low[1],high[1],low[2],high[2],low[3],high[3],draw.blend.mode,draw.blend.source,draw.blend.destination,draw.blend.color_write);
            for (const auto& v:draw.vertices) std::fprintf(stderr,"[movie-vertex] %.2f %.2f %.2f rgba %u %u %u %u uv %.3f %.3f mtx %u\n",v.position[0],v.position[1],v.position[2],v.color[0][0],v.color[0][1],v.color[0][2],v.color[0][3],v.texcoord[0][0],v.texcoord[0][1],v.position_matrix);
        }
#endif
        draws.push_back(std::move(draw));
    }
}
void gx_completed(uint64_t serial) {
    Sched* sched = scheduler;
    if (!sched) return;
    sched->post_external([sched, serial] {
        auto found = finishes.find(serial);
        if (found == finishes.end()) return;
        uint32_t base = found->second; finishes.erase(found); ++completed;
        ppc::Ctx& irq = sched->irq_ctx();
        memory.read(base - 25328, 1);
        ppc::wr8(irq, base - 25328, 1);
        uint32_t callback = read32(base - 25324);
        if (callback) call_guest(irq, callback, {});
        sched->wake_all(base - 25336);
    });
}
GxStats gx_stats() { return {draw_count.load(), vertex_count.load(), submitted.load(), completed.load(), outside_bytes.load(),copies.load(),texture_decodes.load(),geometry_hits.load(),geometry_misses.load(),prompt_annotations.load(),prompt_present.load(),prompt_fallback.load()}; }
}
void hle_GXBegin(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80284FD0, c);
    if (recording) {
        if (recording_primitive()) throw gx::Error(gx::ErrorCode::sequence,0x80284FD0,0,1);
        const uint32_t primitive=c.r[3], vat=c.r[4], count=c.r[5], sdk=read32(c.r[2]-8968);
        const uint32_t callsite=c.lr ? c.lr-4 : 0x80284FD0;
        if (read32(sdk+0x5FC)) uc::call_guest(c,0x80284D58,{});
        if (!read32(sdk)) uc::call_guest(c,0x8028511C,{});
        uc::gx_fifo_write(1,uint8_t(primitive) | uint8_t(vat));
        uc::gx_fifo_write(2,uint16_t(count));
        if (!recording->overflow) {
            auto layout=snapshot(c);
            const size_t expected=gx::encoded_draw_size(layout,uint8_t(primitive),uint8_t(vat),uint16_t(count),callsite);
            const auto [material,uniforms]=material_snapshot(c);
            auto recorded=std::make_unique<RecordedDraw>(RecordedDraw{layout,callsite,uint8_t(primitive),uint8_t(vat),uint16_t(count),expected,{},material,uniforms,texture_selectors(sdk)});
            recorded->payload.reserve(expected);   // filled one write at a time by gx_fifo_write
            recording->native.geometry.push_back(std::move(recorded));
        }
        return;
    }
    auto texture=snapshot_texture(bound_textures[0],palettes);
    capture->begin(gx::share(live_state,snapshot(c)), uint8_t(c.r[3]), uint8_t(c.r[4]), uint16_t(c.r[5]), producer(), c.lr ? c.lr - 4 : 0x80284FD0);
    live_texture=std::move(texture);
    const auto [material,uniforms]=material_snapshot(c);
    live_material=material; gx::share(live_uniforms,uniforms);
    live_texture_wrap=wraps(bound_textures);
    live_texture_maps={};
    const auto maps=texture_mask(material,uniforms);
    for(uint32_t map=1;map<8;++map) if(maps&(1u<<map)) {
        live_texture_maps[map-1]=snapshot_texture(bound_textures[map],palettes);
    }
    gx::share(live_transforms,transforms);
    live_blend=blend_state;
}
void hle_GXSetArray(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802841DC, c);
    uint32_t attr = c.r[3] == 25 ? 10 : c.r[3];
    if (attr < 9 || attr > 24) throw gx::Error(gx::ErrorCode::layout, 0x802841DC, 24, attr);
    if (recording) {
        if (recording_primitive()) throw gx::Error(gx::ErrorCode::sequence,0x802841DC,0,1);
        uc::gx_fifo_write(1,8); uc::gx_fifo_write(1,0xA0+attr-9); uc::gx_fifo_write(4,c.r[4] & 0x3FFFFFFF);
        uc::gx_fifo_write(1,8); uc::gx_fifo_write(1,0xB0+attr-9); uc::gx_fifo_write(4,c.r[5]);
        const gx::Array array{c.r[4],uint8_t(c.r[5])};
        recording->input_state.arrays[attr-9]=array;
        recording->native.geometry.push_back(Resource::ArrayBinding{attr-9,array});
        return;
    }
    state.arrays[attr - 9] = {c.r[4], uint8_t(c.r[5])};
}
void hle_GXSetDrawDone(ppc::Ctx& c) { uc::gx_audit_enter(0x802848AC, c); request_finish(c); }
void hle_GXWaitDrawDone(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80284944, c);
    auto& sched = *scheduler;
    bool interrupts = sched.set_interrupts(false);
    while (std::to_integer<uint8_t>(memory.read(c.r[13] - 25328, 1)[0]) == 0) sched.block_on(c.r[13] - 25336);
    sched.set_interrupts(interrupts);
}
void hle_GXDrawDone(ppc::Ctx& c) { uc::gx_audit_enter(0x80284990, c); request_finish(c); hle_GXWaitDrawDone(c); }
void hle_GXBeginDisplayList(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80288528,c);
    if (recording || capture->active()) throw gx::Error(gx::ErrorCode::sequence,0x80288528,0,1);
    const uint32_t address = c.r[3], capacity = c.r[4], sdk = read32(c.r[2]-8968);
    if ((address & 31) || capacity < 32 || (capacity & 31)) throw gx::Error(gx::ErrorCode::layout,address,32,capacity);
    memory.read(address,capacity); memory.read(sdk,0x600);
    if (read32(sdk+0x5FC)) uc::call_guest(c,0x80284D58,{}); // SDK dirty flush occurs before recording
    auto list = std::make_unique<ListRecording>();
    list->address=address; list->capacity=capacity; list->sdk=sdk; list->owner=producer();
    list->input_state=state;
    list->restore=std::to_integer<uint8_t>(memory.read(sdk+0x5F9,1)[0])!=0;
    if (list->restore) {
        auto shadow=memory.read(sdk,0x600);
        std::copy(shadow.begin(),shadow.end(),list->shadow.begin());
        list->color_shadow=color_uniforms; list->texture_shadow=texture_matrices;
        list->texture_copy_source_shadow=texture_copy_source;
        list->texture_copy_width_shadow=texture_copy_width; list->texture_copy_height_shadow=texture_copy_height;
        list->texture_copy_format_shadow=texture_copy_format;
    }
    list->bytes.reserve(capacity);   // the SDK passes the exact buffer size; gx_fifo_write appends byte by byte
    ppc::wr8(c,sdk+0x5F8,1);
    recording=std::move(list);
}
void hle_GXEndDisplayList(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802885D8,c);
    if (!recording) throw gx::Error(gx::ErrorCode::sequence,0x802885D8,1,0);
    if (recording->owner!=producer()) throw gx::Error(gx::ErrorCode::producer,recording->address,recording->owner,producer());
    if (!recording->overflow && recording_primitive()) {
        const auto& draw=*recorded_draw(recording->native.geometry.back());
        throw gx::Error(gx::ErrorCode::truncated,draw.callsite,draw.expected,draw.payload.size());
    }
    uc::call_guest(c,0x80284504,{}); // verified GXFlush: dirty state followed by 32 NOP bytes
    auto list=std::move(recording);
    const uint32_t length=list->overflow ? 0 : uint32_t(list->bytes.size()) & ~31u;
    if (length) { std::memcpy(c.mem+(list->address & 0x3FFFFFFF),list->bytes.data(),length); gx::note_write_range(list->address,length); }
    if (length) {
        const uint32_t address=list->address & 0x3FFFFFFF;
        for (auto it=resources.begin();it!=resources.end();) {
            if (uint64_t(it->first)<uint64_t(address)+length && uint64_t(address)<uint64_t(it->first)+it->second.bytes.size()) it=resources.erase(it);
            else ++it;
        }
        list->native.bytes.assign(list->bytes.begin(),list->bytes.begin()+length);
        resources.emplace(address,std::move(list->native)).first->second.stamp=gx::watch_stamp();
    }
    uc::gx_audit_recorded_resource(list->address,std::span<const std::byte>(list->bytes).first(length));
    if (list->restore) {
        const uint32_t kept=read32(list->sdk+8);
        std::memcpy(c.mem+(list->sdk & 0x3FFFFFFF),list->shadow.data(),list->shadow.size()); gx::note_write_range(list->sdk,list->shadow.size());
        ppc::wr32(c,list->sdk+8,kept);
        color_uniforms=list->color_shadow; texture_matrices=std::move(list->texture_shadow);
        texture_copy_source=list->texture_copy_source_shadow;
        texture_copy_width=list->texture_copy_width_shadow; texture_copy_height=list->texture_copy_height_shadow;
        texture_copy_format=list->texture_copy_format_shadow;
    }
    ppc::wr8(c,list->sdk+0x5F8,0);
    c.r[3]=length;
}

void hle_GXCallDisplayList(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80288684,c);
    const uint32_t address=c.r[3],length=c.r[4],sdk=read32(c.r[2]-8968);
    if (capture->active() || recording_primitive()) throw gx::Error(gx::ErrorCode::sequence,0x80288684,0,1);
    checked_resource(address,length);
    if (read32(sdk+0x5FC)) uc::call_guest(c,0x80284D58,{});
    if (!read32(sdk)) uc::call_guest(c,0x8028511C,{});
    if (recording) {
        if (producer()!=recording->owner) throw gx::Error(gx::ErrorCode::producer,address,recording->owner,producer());
        uc::gx_fifo_write(1,0x40); uc::gx_fifo_write(4,address); uc::gx_fifo_write(4,length);
        if (recording->overflow) return;
        recording->native.geometry.push_back(Resource::Child{address,length});
        return;
    }
    Imported imported;
    auto bindings=state;
    auto transform_state=transforms;
    auto copy=copy_state;
    auto textures=bound_textures;
    auto loaded=palettes;
    auto blend=blend_state;
    auto colors=material_snapshot(c).second;
    auto matrices=texture_matrices;
    auto versions=copied_textures;
    std::vector<uint32_t> path;
    import_resource(address,length,bindings,transform_state,copy,textures,loaded,blend,colors,matrices,imported,versions,path);
    for (auto& item : imported) {
        if(auto* draw=std::get_if<gx::DrawPacket>(&item)) { vertex_count+=draw->vertices.size(); ++draw_count; draws.push_back(std::move(*draw)); }
        else {
            auto& command=std::get<CopiedTexture>(item); copy_state=command.clear_state;
            submit_batch(false,command.clear,0,std::move(command.request)); ++copies;
        }
    }
    copied_textures=std::move(versions);
    state.arrays=bindings.arrays;
    transforms=transform_state;
    copy_state=copy;
    bound_textures=textures;
    palettes=std::move(loaded);
    blend_state=blend;
    color_uniforms.registers=colors.registers; color_uniforms.konst=colors.konst;
    color_uniforms.lights=colors.lights; color_uniforms.fog=colors.fog; color_uniforms.indirect=colors.indirect;
    texture_matrices=std::move(matrices);
    ppc::wr32(c,sdk+0x254,(read32(sdk+0x254)&~0x70u)|(colors.pixel[3]<<4));
    for(uint32_t i=0;i<4;++i) ppc::wr32(c,sdk+0xB8+i*4,colors.channels[i]);
}

namespace {
void matrix_boundary(uint32_t address) {
    if (capture->active() || recording_primitive()) throw gx::Error(gx::ErrorCode::sequence,address,0,1);
    if (recording && producer()!=recording->owner) throw gx::Error(gx::ErrorCode::producer,address,recording->owner,producer());
}
void projection(ppc::Ctx& c,const std::array<float,6>& p,uint32_t type) {
    matrix_boundary(0x8028873C);
    const auto matrix=gx::make_projection(p,type);
    const uint32_t sdk=read32(c.r[2]-8968);
    memory.read(sdk+0x528,28);
    ppc::wr32(c,sdk+0x528,type);
    for (size_t i=0;i<6;++i) ppc::wr32(c,sdk+0x52C+uint32_t(i*4),std::bit_cast<uint32_t>(p[i]));
    ppc::wr32(c,sdk+0x5FC,read32(sdk+0x5FC)|0x08000000);
    if (recording) { if (!recording->overflow) recording->native.geometry.push_back(Resource::Projection{matrix}); }
    else transforms.projection=matrix;
}
}
void hle_GXSetProjection(ppc::Ctx& c) {
    uc::gx_audit_enter(0x8028873C,c);
    const uint32_t address=c.r[3],type=c.r[4]; memory.read(address,48);
    std::array<float,6> p{};
    const uint32_t offsets[]={0,type==1 ? 12u : 8u,20,type==1 ? 28u : 24u,40,44};
    for (size_t i=0;i<6;++i) p[i]=std::bit_cast<float>(read32(address+offsets[i]));
    projection(c,p,type);
}
void hle_GXSetProjectionv(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802887A0,c);
    const uint32_t address=c.r[3]; memory.read(address,28);
    std::array<float,6> p{};
    for (size_t i=0;i<6;++i) p[i]=std::bit_cast<float>(read32(address+4+uint32_t(i*4)));
    projection(c,p,std::bit_cast<float>(read32(address))==0 ? 0 : 1);
}
void hle_GXLoadPosMtxImm(ppc::Ctx& c) {
    uc::gx_audit_enter(0x8028882C,c); matrix_boundary(0x8028882C);
    const uint32_t row=c.r[4]; const auto matrix=position_matrix(c.r[3]);
    auto checked=transforms; gx::set_position_matrix(checked,row,matrix);
    if (recording) {
        uc::gx_fifo_write(1,0x10); uc::gx_fifo_write(4,0x000B0000|row*4);
        for (const auto& values : matrix) for (float value : values) uc::gx_fifo_write(4,std::bit_cast<uint32_t>(value));
        if (!recording->overflow) recording->native.geometry.push_back(Resource::Position{row,matrix});
    } else transforms=checked;
}
void hle_GXLoadPosMtxIndx(ppc::Ctx& c) {
    uc::gx_audit_enter(0x8028887C,c); matrix_boundary(0x8028887C);
    const uint16_t index=uint16_t(c.r[3]); const uint32_t row=c.r[4];
    if (row>61) throw gx::Error(gx::ErrorCode::layout,0x8028887C,61,row);
    if (recording) {
        uc::gx_fifo_write(1,0x20); uc::gx_fifo_write(4,uint32_t(index)<<16|0xB000|row*4);
        if (!recording->overflow) recording->native.geometry.push_back(Resource::IndexedPosition{index,row});
    } else indexed_position(transforms,state,index,row);
}
void hle_GXLoadNrmMtxIndx3x3(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80288900,c); matrix_boundary(0x80288900);
    const uint16_t index=uint16_t(c.r[3]); const uint32_t row=c.r[4];
    if (row>61) throw gx::Error(gx::ErrorCode::layout,0x80288900,61,row);
    if (recording) {
        uc::gx_fifo_write(1,0x28); uc::gx_fifo_write(4,uint32_t(index)<<16|0x8000|(0x400+row*3));
        if (!recording->overflow) recording->native.geometry.push_back(Resource::IndexedNormal{index,row});
    } else indexed_normal(transforms,state,index,row);
}
void hle_GXSetCurrentMtx(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80288934,c); matrix_boundary(0x80288934);
    const uint32_t sdk=read32(c.r[2]-8968),row=c.r[3]&63;
    ppc::wr32(c,sdk+0x80,(read32(sdk+0x80)&~63u)|row);
    ppc::wr32(c,sdk+0x5FC,read32(sdk+0x5FC)|0x04000000);
    if (recording) { if (!recording->overflow) recording->native.geometry.push_back(Resource::Current{row}); }
    else transforms.current=row;
}

void hle_GXLoadTexObjPreLoaded(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802869A0,c); matrix_boundary(0x802869A0);
    const uint32_t object=c.r[3],region=c.r[4],map=c.r[5],sdk=read32(c.r[2]-8968);
    if (map>7) throw gx::Error(gx::ErrorCode::layout,0x802869A0,7,map);
    memory.read(object,32); memory.read(region,8); memory.read(sdk,0x600);
    const auto flags=std::to_integer<uint8_t>(memory.read(object+31,1)[0]);
    const uint32_t image0=read32(object+8),format=read32(object+20);
    TextureSource source{(read32(object+12)&0xFFFFFF)<<5,{(image0&1023)+1,((image0>>10)&1023)+1,gx::TextureFormat(format),uint8_t(flags&1 ? ((read32(object+4)>>8)&255)/16+1 : 1)}};
    source.wrap=uint8_t(read32(object)&255);
    if((source.wrap&3)>2 || ((source.wrap>>2)&3)>2 || ((source.wrap>>5)&3)>2) throw std::invalid_argument("invalid native texture sampler mode");
    if (format>255 || (format&15)!=((image0>>20)&15)) throw gx::TextureError(gx::TextureErrorCode::descriptor,"inconsistent native GXTexObj format");
    memory.read(source.address,gx::encoded_texture_size(source.desc));
#ifdef UC_TRACE
    static std::vector<uint64_t> plane_logs;
    const uint32_t outer=read32(c.r[1]+20);
    const uint64_t key=uint64_t(outer)<<32 | uint64_t(map)<<28 | uint64_t(format)<<20 | uint64_t(source.desc.width-1)<<10 | (source.desc.height-1);
    if (plane_logs.size()<80 && std::find(plane_logs.begin(),plane_logs.end(),key)==plane_logs.end()) {
        plane_logs.push_back(key);
        std::fprintf(stderr,"[texture-plane] map %u address %08X %ux%u format %u caller %08X outer %08X\n",map,source.address,source.desc.width,source.desc.height,format,c.lr,outer);
    }
#endif
    const uint32_t tables[]={28936,28928,28920,28912,28904,28896};
    uint32_t words[]={read32(object),read32(object+4),image0,read32(region),read32(region+4),read32(object+12)};
    for (size_t i=0;i<6;++i) words[i]=(words[i]&0xFFFFFF)|(uint32_t(std::to_integer<uint8_t>(memory.read(c.r[13]-tables[i]+map,1)[0]))<<24);
    // Keep verified SDK byte identity for native resource version checks; no BP interpretation.
    for (auto word : words) { uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,word); }
    ppc::wr32(c,object,words[0]); ppc::wr32(c,object+4,words[1]); ppc::wr32(c,object+8,words[2]); ppc::wr32(c,object+12,words[5]);
    ppc::wr32(c,region,words[3]); ppc::wr32(c,region+4,words[4]);
    if (!(flags&2)) {
        const auto palette_region=uc::call_guest(c,read32(sdk+0x51C),{read32(object+24)});
        memory.read(palette_region,16);
        source.palette=palette_region&0x3FFFFFFF;
        const auto tag=std::to_integer<uint8_t>(memory.read(c.r[13]-28888+map,1)[0]);
        const auto word=(read32(palette_region+4)&0xFFFFFF)|(uint32_t(tag)<<24);
        ppc::wr32(c,palette_region+4,word);
        uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,word);
    }
    ppc::wr32(c,sdk+0x564+map*4,words[2]); ppc::wr32(c,sdk+0x584+map*4,words[0]);
    ppc::wr32(c,sdk+0x5FC,read32(sdk+0x5FC)|1); ppc::wr16(c,sdk+2,0);
    if (recording) { if (!recording->overflow) recording->native.geometry.push_back(Resource::TextureBinding{map,source}); }
    else bound_textures[map]=source;
}
void hle_GXLoadTlut(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80286B80,c); matrix_boundary(0x80286B80);
    const uint32_t object=c.r[3],name=c.r[4],sdk=read32(c.r[2]-8968);
    memory.read(object,12);
    const uint32_t region=uc::call_guest(c,read32(sdk+0x51C),{name});
    memory.read(region,16);
    const uint32_t format=read32(object),image=read32(object+4),count_word=read32(object+8);
    const PaletteSource source{(image&0xFFFFFF)<<5,count_word>>16,gx::PaletteFormat((format>>10)&3)};
    auto loaded=load_palette(source);
    uc::call_guest(c,0x80287720,{});
    uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,image);
    uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,read32(region));
    uc::call_guest(c,0x80287720,{});
    const uint32_t binding=(format&~1023u)|(read32(region)&1023);
    ppc::wr32(c,region+4,binding); ppc::wr32(c,region+8,image); ppc::wr32(c,region+12,count_word);
    ppc::wr32(c,object,binding);
    if (recording) { if (!recording->overflow) recording->native.geometry.push_back(Resource::PaletteLoad{region&0x3FFFFFFF,source}); }
    else palettes[region&0x3FFFFFFF]=std::move(loaded);
}
void hle_GXSetCopyClear(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802855C8,c); matrix_boundary(0x802855C8);
    const auto bytes=memory.read(c.r[3],4);
    CopyState next;
    for (size_t i=0;i<4;++i) next.color[i]=std::to_integer<uint8_t>(bytes[i]);
    next.depth=c.r[4]&0xFFFFFF;
    const uint32_t sdk=read32(c.r[2]-8968); memory.read(sdk+2,2);
    if (recording) {
        const uint32_t words[]={0x4F000000u|next.color[0]|uint32_t(next.color[3])<<8,0x50000000u|next.color[2]|uint32_t(next.color[1])<<8,0x51000000u|next.depth};
        for (uint32_t word : words) { uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,word); }
        if (!recording->overflow) recording->native.geometry.push_back(Resource::CopyClear{next});
    } else copy_state=next;
    ppc::wr16(c,sdk+2,0);
}
void hle_GXCopyDisp(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80285838,c);
    if (recording) throw gx::Error(gx::ErrorCode::sequence,0x80285838,0,1); // TD-034: recorded copies need a verified native resource boundary
    capture->finish();
    const uint32_t xfb=c.r[3];
    if (xfb&31) throw gx::Error(gx::ErrorCode::layout,xfb,32,xfb&31);
    memory.read(xfb,32);
    const uint32_t sdk=read32(c.r[2]-8968);
    const bool clear=c.r[4]!=0;
    ppc::wr32(c,sdk+0x23C,(read32(sdk+0x23C)&~0xFF000800u)|0x52004000u|(clear ? 0x800u : 0));
    memory.read(sdk+2,2); ppc::wr16(c,sdk+2,0);
    const auto origin=read32(sdk+0x230),extent=read32(sdk+0x234);
    std::array<uint32_t,4> source{0,0,640,480};
    if ((origin>>24)==0x49 && (extent>>24)==0x4A) source={origin&1023,(origin>>10)&1023,(extent&1023)+1,((extent>>10)&1023)+1};
    if(source[0]>=640 || source[2]>640-source[0] || source[1]>=480 || source[3]>480-source[1]) throw std::invalid_argument("invalid native display source rectangle");
    submit_batch(true,clear,xfb,{},source,uc::vi_stats().black); ++copies;
}
void hle_GXSetTexCopySrc(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80285334,c); capture->finish();
    const auto x=c.r[3],y=c.r[4],width=c.r[5],height=c.r[6],sdk=read32(c.r[2]-8968);
    if(!width || !height || x>=640 || width>640-x || y>=480 || height>480-y) throw std::invalid_argument("invalid native EFB texture source rectangle");
    memory.read(sdk+0x240,8);
    texture_copy_source={x,y,width,height};
    ppc::wr32(c,sdk+0x240,0x49000000u|x|y<<10);
    ppc::wr32(c,sdk+0x244,0x4A000000u|(width-1)|(height-1)<<10);
}
void hle_GXSetTexCopyDst(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80285398,c); capture->finish();
    const auto width=c.r[3],height=c.r[4],format=c.r[5],half=c.r[6],sdk=read32(c.r[2]-8968);
    if(!width || !height || width>1024 || height>1024 || (format!=4 && format!=6 && format!=0x27) || half>1) {
        std::fprintf(stderr,"[efb-destination-invalid] %ux%u format %08X half %u caller %08X\n",width,height,format,half,c.lr);
        throw std::invalid_argument("unsupported native EFB texture destination");
    }
    memory.read(sdk+0x248,9);
    // Original SDK tile stride in32byte units: RGBA8=4x4/64; RGB565=4x4/32; A8=8x4/32.
    const auto stride=format==0x27 ? (width+7)/8 : ((width+3)/4)*(format==6 ? 2 : 1);
    const auto copy_type=format==0x27 ? 0x70u : format<<4;
    ppc::wr32(c,sdk+0x248,0x4D000000u|stride);
    ppc::wr32(c,sdk+0x24C,(read32(sdk+0x24C)&~0x18278u)|0x10000u|half<<9|copy_type);
    ppc::wr8(c,sdk+0x250,0);
    texture_copy_width=width; texture_copy_height=height;
    texture_copy_format=format;
}
void hle_GXCopyTex(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80285974,c); capture->finish();
    const auto address=c.r[3],sdk=read32(c.r[2]-8968);
    if((address&31) || c.r[4]>1 || !texture_copy_source[2] || !texture_copy_width || !texture_copy_height) throw std::invalid_argument("invalid native EFB texture copy request");
    memory.read(address,gx::encoded_texture_size({texture_copy_width,texture_copy_height,texture_copy_format==0x27 ? gx::TextureFormat::i8 : gx::TextureFormat(texture_copy_format),1}));
    memory.read(sdk,0x254);
    const auto control=(read32(sdk+0x24C)&~0xFF004800u)|0x52000000u|c.r[4]<<11;
    if(profile_mesh && mesh_profile_count && copy_profile_count<24) {
        ++copy_profile_count;
        std::fprintf(stderr,"[scene-copy] clear %u source %u:%u:%u:%u target %u:%u color %u:%u:%u:%u blend %08X depth %08X\n",c.r[4],texture_copy_source[0],texture_copy_source[1],texture_copy_source[2],texture_copy_source[3],texture_copy_width,texture_copy_height,copy_state.color[0],copy_state.color[1],copy_state.color[2],copy_state.color[3],read32(sdk+0x220),read32(sdk+0x228));
    }
    if(recording) {
        if(producer()!=recording->owner || recording_primitive()) throw std::invalid_argument("invalid recorded EFB texture copy boundary");
        if(c.r[4]) for(auto word:{read32(sdk+0x228)|15,read32(sdk+0x220)&~3u}) { uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,word); }
        for(auto word:{read32(sdk+0x240),read32(sdk+0x244),read32(sdk+0x248),0x4B000000u|((address&0x3FFFFFFF)>>5),control}) { uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,word); }
        if(c.r[4]) for(auto word:{read32(sdk+0x228),read32(sdk+0x220)}) { uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,word); }
        if(!recording->overflow) recording->native.geometry.push_back(Resource::TextureCopy{texture_copy_source,address,texture_copy_width,texture_copy_height,c.r[4]!=0,texture_copy_format});
        ppc::wr32(c,sdk+0x24C,control); ppc::wr16(c,sdk+2,0);
        return;
    }
    auto target=make_copy_texture(address,texture_copy_width,texture_copy_height,texture_copy_format);
    const auto [entry,inserted]=copied_textures.try_emplace(address&0x3FFFFFFF);
    try { submit_batch(false,c.r[4]!=0,0,gx::TextureCopy{texture_copy_source,target}); }
    catch(...) { if(inserted) copied_textures.erase(entry); throw; }
    entry->second=std::move(target);
    ppc::wr32(c,sdk+0x24C,control);
    ppc::wr16(c,sdk+2,0); ++copies;
}
namespace {
void publish_blend(ppc::Ctx& c,uint32_t sdk,uint32_t word) {
    uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,word);
    ppc::wr32(c,sdk+0x220,word); ppc::wr16(c,sdk+2,0);
    gx::BlendState blend{uint8_t(word&0x800 ? 3 : word&2 ? 2 : word&1 ? 1 : 0),uint8_t((word>>8)&7),uint8_t((word>>5)&7),uint8_t((word>>12)&15),bool(word&8),bool(word&16)};
    if (recording) { if (!recording->overflow) recording->native.geometry.push_back(Resource::Blend{blend}); }
    else blend_state=blend;
}
}
void hle_GXSetBlendMode(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80288274,c); matrix_boundary(0x80288274);
    if (c.r[3]>3 || c.r[4]>7 || c.r[5]>7 || c.r[6]>15) throw gx::Error(gx::ErrorCode::layout,0x80288274,3,c.r[3]);
    const auto sdk=read32(c.r[2]-8968),mode=c.r[3];
    const auto word=(read32(sdk+0x220)&~0xF7E3u)|(mode&1)|(mode==2 ? 2u : 0u)|(mode==3 ? 0x800u : 0u)|(c.r[4]<<8)|(c.r[5]<<5)|(c.r[6]<<12);
    publish_blend(c,sdk,word);
}
void hle_GXSetColorUpdate(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802882C4,c); matrix_boundary(0x802882C4);
    if (c.r[3]>1) throw gx::Error(gx::ErrorCode::layout,0x802882C4,1,c.r[3]);
    const auto sdk=read32(c.r[2]-8968); publish_blend(c,sdk,(read32(sdk+0x220)&~8u)|(c.r[3]<<3));
}
void hle_GXSetAlphaUpdate(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802882F0,c); matrix_boundary(0x802882F0);
    if (c.r[3]>1) throw gx::Error(gx::ErrorCode::layout,0x802882F0,1,c.r[3]);
    const auto sdk=read32(c.r[2]-8968); publish_blend(c,sdk,(read32(sdk+0x220)&~16u)|(c.r[3]<<4));
}
void hle_ResTevApply(ppc::Ctx& c) {
    matrix_boundary(0x80366E78);
    const auto object=read32(c.r[3]),sdk=read32(c.r[2]-8968);
    memory.read(object,32); memory.read(sdk,0x600);
    const auto count=std::to_integer<uint8_t>(memory.read(object+12,1)[0]);
    const auto bytes=memory.read(object+32,gx::tev_material_size(count));
    gx::TevMaterial parsed;
    try { parsed=gx::import_tev_material(count,bytes); }
    catch(const std::invalid_argument&) {
        std::fprintf(stderr,"[native-tev-invalid] object %08X stages %u prefix",object,count);
        for(auto byte:bytes) std::fprintf(stderr," %02X",std::to_integer<unsigned>(byte));
        std::fprintf(stderr,"\n"); throw;
    }
    color_uniforms.indirect[9]=parsed.indirect_order;
    ppc::wr32(c,sdk+0x254,(read32(sdk+0x254)&~0x3C00u)|((count-1)<<10));
    for(size_t i=0;i<4;++i) {
        for(size_t lane=0;lane<2;++lane) {
            const auto at=sdk+0x200+uint32_t(i*8+lane*4);
            ppc::wr32(c,at,(read32(at)&~15u)|((parsed.swaps[i]>>(lane*4))&15));
        }
    }
    for(uint32_t i=0;i<count;++i) {
        const auto& stage=parsed.stage[i];
        indirect_stages[i]=stage.indirect;
        ppc::wr32(c,sdk+0x180+i*4,(0xC0+i*2)<<24|stage.color);
        ppc::wr32(c,sdk+0x1C0+i*4,(0xC1+i*2)<<24|stage.alpha);
        const auto at=sdk+0x150+i/2*4,shift=(i%2)*12;
        ppc::wr32(c,at,(read32(at)&~(0xFFFu<<shift))|(stage.order<<shift));
        const auto selector=sdk+0x200+i/2*4,bits=4+(i%2)*10;
        ppc::wr32(c,selector,(read32(selector)&~(1023u<<bits))|(stage.konst<<bits));
    }
}
void hle_ResColorApply(ppc::Ctx& c) {
    matrix_boundary(0x80367024);
    const auto bytes=memory.read(read32(c.r[3]),128);
    const auto sdk=read32(c.r[2]-8968);
    memory.read(sdk,4);
    gx::ColorMaterial parsed;
    try { parsed=gx::import_color_material(bytes); }
    catch(const std::invalid_argument&) {
        std::fprintf(stderr,"[native-color-invalid] hex");
        for(auto byte:bytes) std::fprintf(stderr," %02X",std::to_integer<unsigned>(byte));
        std::fprintf(stderr,"\n"); throw;
    }
    if(recording) {
        uc::gx_fifo_write(1,0x40); uc::gx_fifo_write(4,read32(c.r[3])); uc::gx_fifo_write(4,128);
    }
    for(uint32_t i=0;i<3;++i) {
        color_uniforms.registers[i+1]=parsed.registers[i];
        if(recording && !recording->overflow) recording->native.geometry.push_back(Resource::TevColor{i+1,false,parsed.registers[i]});
    }
    for(uint32_t i=0;i<4;++i) {
        color_uniforms.konst[i]=parsed.konst[i];
        if(recording && !recording->overflow) recording->native.geometry.push_back(Resource::TevColor{i,true,parsed.konst[i]});
    }
    ppc::wr16(c,sdk+2,0);
}
void hle_ResPixelApply(ppc::Ctx& c) {
    matrix_boundary(0x80366FF4);
    const auto object=read32(c.r[3]),sdk=read32(c.r[2]-8968);
    const auto pixel=gx::import_pixel_material(memory.read(object,32));
    memory.read(sdk,0x600);
    color_uniforms.pixel={pixel.alpha,pixel.depth,pixel.destination_alpha,0};
    ppc::wr32(c,sdk+0x228,0x40000000|pixel.depth);
    ppc::wr32(c,sdk+0x224,0x42000000|pixel.destination_alpha);
    const auto blend=(read32(sdk+0x220)&~0xFFE3u)|(pixel.blend&0xFFE3u);
    publish_blend(c,sdk,blend);
}
void hle_GXSetAlphaCompare(ppc::Ctx& c) {
    matrix_boundary(0x80287B80);
    if(c.r[3]>7 || c.r[4]>255 || c.r[5]>3 || c.r[6]>7 || c.r[7]>255) throw std::invalid_argument("invalid native alpha compare");
    const auto word=c.r[4]|c.r[7]<<8|c.r[3]<<16|c.r[6]<<19|c.r[5]<<22;
    color_uniforms.pixel[0]=word;
    uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,0xF3000000|word);
    ppc::wr16(c,read32(c.r[2]-8968)+2,0);
}
namespace {
std::span<const std::byte> mesh_resource(ppc::Ctx& c,uint32_t descriptor,uint32_t skip) {
    const auto object=read32(c.r[3]); memory.read(object,48);
    const auto meta=object+descriptor,size=read32(meta+4),offset=read32(meta+8);
    const int64_t address=int64_t(meta)+int32_t(offset)+skip;
    if(!offset || size<skip || address<0 || address>UINT32_MAX) throw std::invalid_argument("invalid native mesh resource span");
    return memory.read(uint32_t(address),size-skip);
}
}
void hle_ResMeshSetup(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80366D3C,c);
    matrix_boundary(0x80366D3C);
    if(recording) throw std::invalid_argument("recorded native mesh setup requires implementation");
    const auto bytes=mesh_resource(c,24,c.r[5] ? 32 : 0);
    if(bytes.size()<96 || bytes.size()>224 || (bytes.size()&31)) {
        std::fprintf(stderr,"[mesh-setup-variant] object %08X size %zu skip %u hex",read32(c.r[3]),bytes.size(),c.r[5] ? 32u : 0u);
        for(auto byte:bytes.first(std::min<size_t>(bytes.size(),512))) std::fprintf(stderr," %02X",std::to_integer<unsigned>(byte));
        std::fprintf(stderr,"\n");
    }
    const auto parsed=gx::import_mesh_setup(bytes);
    const auto sdk=read32(c.r[2]-8968); memory.read(sdk,0x600);
    if(parsed.descriptors) { ppc::wr32(c,sdk+0x14,parsed.low); ppc::wr32(c,sdk+0x18,parsed.high); }
    ppc::wr32(c,sdk+0x1C,parsed.vat[0]); ppc::wr32(c,sdk+0x3C,parsed.vat[1]); ppc::wr32(c,sdk+0x5C,parsed.vat[2]);
    for(size_t i=0;i<12;++i) if(parsed.arrays&(1u<<i)) state.arrays[i]={parsed.addresses[i],uint8_t(parsed.strides[i])};
}
namespace {
uint32_t guest_address(const std::byte* host) {
    if(host>=memory.mem1.data() && host<memory.mem1.data()+memory.mem1.size()) return uint32_t(host-memory.mem1.data());
    return 0x10000000u+uint32_t(host-memory.mem2.data());
}
std::shared_ptr<const CachedMesh> native_mesh(std::span<const std::byte> bytes,const gx::State& layout) {
    const auto key=reinterpret_cast<uintptr_t>(bytes.data());
    auto found=mesh_index.find(key);
    if(found!=mesh_index.end()) {
        const auto& cached=*found->second->mesh;
        // The mesh description and its source arrays are re-read only when a guest store touched their pages since the last comparison,
        // and anyway every 256th hit (net for native writers that miss the barrier).
        const uint32_t guest=guest_address(bytes.data());
        bool clean=cached.skips<256 && cached.bytes.size()==bytes.size() && !gx::written_since(guest,bytes.size(),cached.stamp);
        if(clean) for(const auto& source:cached.sources) if(gx::written_since(source.address,source.bytes.size(),cached.stamp)) { clean=false; break; }
        bool valid=cached.layout.descriptors==layout.descriptors && (clean || gx::same_bytes(bytes,cached.bytes));
        for(size_t attr=0;valid && attr<21;++attr) if(layout.descriptors[attr]!=gx::Mode::none) {
            valid=cached.layout.formats[0][attr]==layout.formats[0][attr];
            if(attr>=9 && (layout.descriptors[attr]==gx::Mode::index8 || layout.descriptors[attr]==gx::Mode::index16)) valid&=cached.layout.arrays[attr-9]==layout.arrays[attr-9];
        }
        if(valid && !clean) for(const auto& source:cached.sources) {
            const auto current=memory.read(source.address,source.bytes.size());
            if(!gx::same_bytes(current,source.bytes)) { valid=false; break; }
        }
        if(valid) {
            if(clean) ++cached.skips; else { cached.stamp=gx::watch_stamp(); cached.skips=0; }
            ++geometry_hits; mesh_cache.splice(mesh_cache.begin(),mesh_cache,found->second); return found->second->mesh;
        }
    }
    ++geometry_misses;
    auto result=std::make_shared<CachedMesh>(); result->layout=layout;
    result->bytes.assign(bytes.begin(),bytes.end());
    std::array<std::optional<gx::Vertices::SourceRange>,12> sources{};
    gx::Vertices decoder(memory,true);
    for(size_t at=0;at<bytes.size();) {
        const auto opcode=std::to_integer<uint8_t>(bytes[at++]);
        if(!opcode) continue;
        if(opcode==0x30) {
            if(bytes.size()-at<4) throw std::invalid_argument("truncated native mesh matrix binding");
            uint32_t word=0; for(auto byte:bytes.subspan(at,4)) word=word<<8|std::to_integer<uint8_t>(byte); at+=4;
            const auto target=word&0xFFF,format=(word>>12)&15;
            const auto id=target>=0x500 ? 64+(target-0x500)/4 : target/4;
            if((format!=11 && format!=7) || (target&3) || (target<0x500 && id>61) || id>125) throw std::invalid_argument("invalid native mesh texture matrix binding");
            result->operations.push_back(MeshTexture{uint16_t(word>>16),id,format+1});
            continue;
        }
        if(opcode==0x20 || opcode==0x28) {
            if(bytes.size()-at<4) throw std::invalid_argument("truncated native mesh matrix binding");
            uint32_t word=0; for(auto byte:bytes.subspan(at,4)) word=word<<8|std::to_integer<uint8_t>(byte); at+=4;
            const auto target=word&0xFFF;
            if(opcode==0x20) {
                if(((word>>12)&15)!=11 || (target&3)) throw std::invalid_argument("invalid native mesh position matrix binding");
                result->operations.push_back(MeshPosition{uint16_t(word>>16),target/4});
            } else {
                if(((word>>12)&15)!=8 || target<0x400 || target>0x4B7 || (target-0x400)%3) throw std::invalid_argument("invalid native mesh normal matrix binding");
                result->operations.push_back(MeshNormal{uint16_t(word>>16),(target-0x400)/3});
            }
            continue;
        }
        if((opcode&7)!=0 || at+2>bytes.size()) throw std::invalid_argument("unsupported native mesh primitive record");
        const auto count=uint16_t(std::to_integer<uint8_t>(bytes[at])<<8|std::to_integer<uint8_t>(bytes[at+1])); at+=2;
        if(!count) continue;
        if(opcode!=0x80 && opcode!=0x90 && opcode!=0x98 && opcode!=0xA0) throw std::invalid_argument("unsupported native mesh triangle primitive");
        const auto length=gx::encoded_draw_size(layout,opcode,0,count,0x80366E14);
        if(length>bytes.size()-at) throw gx::Error(gx::ErrorCode::truncated,0x80366E14,length,bytes.size()-at);
        decoder.begin(layout,opcode,0,count,producer(),0x80366E14);
        decoder.write_attribute(producer(),bytes.subspan(at,length)); at+=length;
        auto draw=decoder.take_draw();
        for(size_t i=0;i<sources.size();++i) if(const auto& incoming=decoder.sources()[i];incoming) {
            auto& range=sources[i];
            const auto begin=range ? std::min(uint64_t(range->address),uint64_t(incoming->address)) : incoming->address;
            const auto end=range ? std::max(uint64_t(range->address)+range->size,uint64_t(incoming->address)+incoming->size) : uint64_t(incoming->address)+incoming->size;
            range=gx::Vertices::SourceRange{uint32_t(begin),size_t(end-begin)};
        }
        auto* previous=result->operations.empty() ? nullptr : std::get_if<MeshGeometry>(&result->operations.back());
        if(previous) {
            auto& merged=*previous->buffers;
            if(draw.vertices.size()>size_t(INT32_MAX)-merged.vertices.size()) throw std::invalid_argument("native mesh batch exceeds vertex index limits");
            const auto offset=uint32_t(merged.vertices.size());
            merged.vertices.insert(merged.vertices.end(),draw.vertices.begin(),draw.vertices.end());
            for(auto index:draw.indices) merged.indices.push_back(offset+index);
        } else result->operations.push_back(MeshGeometry{std::make_shared<gx::MeshBuffers>(gx::MeshBuffers{std::move(draw.vertices),std::move(draw.indices)})});
    }
    for(const auto& range:sources) if(range) {
        const auto current=memory.read(range->address,range->size);
        result->sources.push_back({range->address,{current.begin(),current.end()}});
    }
    result->stamp=gx::watch_stamp();
    result->size=sizeof(CachedMesh)+result->bytes.capacity()+result->operations.capacity()*sizeof(decltype(result->operations)::value_type)+result->sources.capacity()*sizeof(CachedMesh::Source);
    for(const auto& source:result->sources) result->size+=source.bytes.capacity();
    for(const auto& operation:result->operations) if(const auto* geometry=std::get_if<MeshGeometry>(&operation)) result->size+=geometry->buffers->vertices.capacity()*sizeof(gx::Vertex)+geometry->buffers->indices.capacity()*sizeof(uint32_t);
    if(found!=mesh_index.end()) { mesh_cache_bytes-=found->second->mesh->size; mesh_cache.erase(found->second); mesh_index.erase(found); }
    if(result->size<=mesh_cache_budget) {
        while(mesh_cache_bytes>mesh_cache_budget-result->size) { mesh_cache_bytes-=mesh_cache.back().mesh->size; mesh_index.erase(mesh_cache.back().key); mesh_cache.pop_back(); }
        mesh_cache.push_front({key,result});
        try { mesh_index.emplace(key,mesh_cache.begin()); }
        catch(...) { mesh_cache.pop_front(); throw; }
        mesh_cache_bytes+=result->size;
    }
    return result;
}
}
void hle_ResMeshDraw(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80366E14,c);
    matrix_boundary(0x80366E14);
    if(recording) throw std::invalid_argument("recorded native mesh draw requires implementation");
    scheduler->safe_point(); // Service audio/VI between complete native mesh calls.
    const auto bytes=mesh_resource(c,36,0);
    const auto layout=snapshot(c);
    const auto [material,uniforms]=material_snapshot(c);
    const auto misses_before=geometry_misses.load();
    const auto cached=native_mesh(bytes,layout);
    std::array<std::shared_ptr<const gx::TexturePixels>,8> textures{};
    const bool profile_this_mesh=profile_mesh && mesh_profile_count<256 && geometry_misses.load()!=misses_before;
    if(profile_this_mesh) {
        ++mesh_profile_count;
        const auto sdk=read32(c.r[2]-8968);
        std::fprintf(stderr,"[mesh-material] object %08X stages %u channels %u descriptors %08X:%08X ambient %08X:%08X material %08X:%08X control %08X:%08X:%08X:%08X\n",c.r[3],material.stages,(read32(sdk+0x254)>>4)&7,read32(sdk+0x14),read32(sdk+0x18),read32(sdk+0xA8),read32(sdk+0xAC),read32(sdk+0xB0),read32(sdk+0xB4),read32(sdk+0xB8),read32(sdk+0xBC),read32(sdk+0xC0),read32(sdk+0xC4));
        for(uint32_t i=0;i<material.stages;++i) std::fprintf(stderr,"[mesh-stage] %u color %06X alpha %06X order %03X konst %03X indirect %06X\n",i,material.stage[i].color,material.stage[i].alpha,material.stage[i].order,material.stage[i].konst,material.stage[i].indirect);
        for(uint32_t i=0;i<8;++i) {
            const auto& light=uniforms.lights[i];
            std::fprintf(stderr,"[mesh-light] %u rgba %.6g:%.6g:%.6g:%.6g cosine %.6g:%.6g:%.6g distance %.6g:%.6g:%.6g position %.6g:%.6g:%.6g direction %.6g:%.6g:%.6g\n",i,light.color[0],light.color[1],light.color[2],light.color[3],light.cosine[0],light.cosine[1],light.cosine[2],light.distance[0],light.distance[1],light.distance[2],light.position[0],light.position[1],light.position[2],light.direction[0],light.direction[1],light.direction[2]);
        }
        for(uint32_t i=0;i<8;++i) if(bound_textures[i]) {
            const auto& rows=uniforms.tex_rows;
            std::fprintf(stderr,"[mesh-map] %u source %08X extent %u:%u format %u texgen %08X rows %.6g:%.6g:%.6g:%.6g %.6g:%.6g:%.6g:%.6g\n",i,bound_textures[i]->address,bound_textures[i]->desc.width,bound_textures[i]->desc.height,unsigned(bound_textures[i]->desc.format),uniforms.tex_gens[i],rows[i*3][0],rows[i*3][1],rows[i*3][2],rows[i*3][3],rows[i*3+1][0],rows[i*3+1][1],rows[i*3+1][2],rows[i*3+1][3]);
        }
    }
    const auto maps=texture_mask(material,uniforms);
    for(uint32_t map=0;map<8;++map) if(maps&(1u<<map)) {
        if(!textures[map]) textures[map]=snapshot_texture(bound_textures[map],palettes);
    }
    if(profile_this_mesh && mesh_profile_count==1) for(size_t map=0;map<2;++map) {
        if(!textures[map] || textures[map]->gpu_only || textures[map]->levels.empty()) continue;
        const auto& level=textures[map]->levels[0];
        try {
            const auto directory=std::filesystem::path(L"S:\\Games\\Umbrella Chronicles\\debug");
            std::filesystem::create_directories(directory);
            const auto path=directory/("mesh-map-"+std::to_string(map)+"-"+std::to_string(uc::now_ticks())+".ppm");
            std::ofstream file(path,std::ios::binary);
            file.exceptions(std::ios::badbit|std::ios::failbit);
            file<<"P6\n"<<level.width<<' '<<level.height<<"\n255\n";
            std::vector<uint8_t> rgb; rgb.reserve(size_t(level.width)*level.height*3);
            for(size_t i=0;i<level.rgba.size();i+=4) rgb.insert(rgb.end(),level.rgba.begin()+i,level.rgba.begin()+i+3);
            file.write(reinterpret_cast<const char*>(rgb.data()),std::streamsize(rgb.size())); file.close();
            std::fprintf(stderr,"[mesh-texture-dump] %s\n",path.string().c_str());
        } catch(const std::exception& error) { std::fprintf(stderr,"mesh texture dump failed: %s\n",error.what()); }
    }
    const size_t first_draw=draws.size();   // appended in place; removed again if the mesh fails midway
    auto mesh_transforms=transforms;
    auto mesh_uniforms=uniforms;
    auto mesh_matrices=texture_matrices;
    const auto selectors=texture_selectors(read32(c.r[2]-8968));
    try {
        for(const auto& operation:cached->operations) {
            if(const auto* texture=std::get_if<MeshTexture>(&operation)) {
                indexed_texture(mesh_matrices,layout,texture->index,texture->id,texture->words);
                resolve_texture_matrices(mesh_uniforms,selectors,mesh_matrices);
                continue;
            }
            if(const auto* position=std::get_if<MeshPosition>(&operation)) {
                indexed_position(mesh_transforms,layout,position->index,position->row);
            } else if(const auto* normal=std::get_if<MeshNormal>(&operation)) {
                indexed_normal(mesh_transforms,layout,normal->index,normal->row);
            } else {
                const auto& geometry=std::get<MeshGeometry>(operation);
                auto& draw=draws.emplace_back();
                draw.callsite=0x80366E14; draw.primitive=0x90; draw.vat=0; draw.state=gx::share(mesh_state,layout);
                draw.mesh=geometry.buffers;
                draw.transforms=gx::share(mesh_transforms_block,mesh_transforms); draw.material=material; draw.uniforms=gx::share(mesh_uniforms_block,mesh_uniforms); draw.blend=blend_state; draw.texture=textures[0];
                draw.texture_wrap=wraps(bound_textures);
                std::copy(textures.begin()+1,textures.end(),draw.texture_maps.begin());
            }
        }
    } catch(...) { draws.erase(draws.begin()+std::ptrdiff_t(first_draw),draws.end()); throw; }
    if(profile_this_mesh && draws.size()>first_draw) {
        std::array<uint8_t,4> low_color{255,255,255,255},high_color{};
        for(size_t d=first_draw;d<draws.size();++d) for(const auto& vertex:draws[d].vertex_view()) for(size_t channel=0;channel<4;++channel) {
            low_color[channel]=std::min(low_color[channel],vertex.color[0][channel]); high_color[channel]=std::max(high_color[channel],vertex.color[0][channel]);
        }
        std::fprintf(stderr,"[mesh-color-range] object %08X rgba %u:%u %u:%u %u:%u %u:%u mode %u\n",c.r[3],low_color[0],high_color[0],low_color[1],high_color[1],low_color[2],high_color[2],low_color[3],high_color[3],unsigned(layout.descriptors[11]));
        for(size_t map=0;map<2;++map) {
            std::array<float,2> low{INFINITY,INFINITY},high{-INFINITY,-INFINITY};
            for(size_t d=first_draw;d<draws.size();++d) for(const auto& vertex:draws[d].vertex_view()) for(size_t axis=0;axis<2;++axis) {
                low[axis]=std::min(low[axis],vertex.texcoord[map][axis]); high[axis]=std::max(high[axis],vertex.texcoord[map][axis]);
            }
            std::fprintf(stderr,"[mesh-uv-range] map %zu UV %.6g:%.6g %.6g:%.6g wrap %u\n",map,low[0],high[0],low[1],high[1],bound_textures[map] ? bound_textures[map]->wrap : 0);
        }
    }
    for(size_t d=first_draw;d<draws.size();++d) { vertex_count+=draws[d].vertex_view().size(); ++draw_count; }
    transforms=mesh_transforms; texture_matrices=mesh_matrices;
}
namespace {
void native_tev_color(ppc::Ctx& c,bool signed_color,bool konst) {
    matrix_boundary(konst ? 0x802879CC : signed_color ? 0x80287968 : 0x80287908);
    const auto id=c.r[3],pointer=c.r[4],sdk=read32(c.r[2]-8968);
    if(id>3) throw gx::Error(gx::ErrorCode::layout,pointer,3,id);
    memory.read(pointer,signed_color ? 8 : 4); memory.read(sdk,4);
    std::array<float,4> value{}; std::array<uint32_t,4> channels{};
    for(uint32_t i=0;i<4;++i) {
        const auto v=signed_color ? int32_t(int16_t(ppc::rd16(c,pointer+i*2))) : int32_t(ppc::rd8(c,pointer+i));
        if(signed_color && (v< -1024 || v>1023)) throw gx::Error(gx::ErrorCode::layout,pointer,1023,size_t(v));
        value[i]=v/255.f; channels[i]=uint32_t(v)&0x7FF;
    }
    if(konst) color_uniforms.konst[id]=value; else color_uniforms.registers[id]=value;
    const auto low=((0xE0+id*2)<<24)|(konst ? 0x800000u : 0u)|channels[0]|channels[3]<<12;
    const auto high=((0xE1+id*2)<<24)|(konst ? 0x800000u : 0u)|channels[2]|channels[1]<<12;
    uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,low);
    for(size_t i=0;i<(konst ? 1 : 3);++i) { uc::gx_fifo_write(1,0x61); uc::gx_fifo_write(4,high); }
    if(recording && !recording->overflow) recording->native.geometry.push_back(Resource::TevColor{id,konst,value});
    ppc::wr16(c,sdk+2,0);
}
}
void hle_GXSetTevColor(ppc::Ctx& c) { native_tev_color(c,false,false); }
void hle_GXSetTevColorS10(ppc::Ctx& c) { native_tev_color(c,true,false); }
void hle_GXSetTevKColor(ppc::Ctx& c) { native_tev_color(c,false,true); }
void hle_GXSetTevDirect(ppc::Ctx& c) {
    matrix_boundary(0x80287650);
    const auto stage=c.r[3],sdk=read32(c.r[2]-8968);
    if(stage>15) throw gx::Error(gx::ErrorCode::layout,0x80287650,15,stage);
    native_bp((0x10+stage)<<24);
    ppc::wr16(c,sdk+2,0);
}
// Fog and indirect setters, call-level replacements of the RBUE08 SDK bodies (bit layouts from their disassembly).
// The FIFO words stay byte-identical for display lists; native state comes from the arguments, never from FIFO decoding.
void hle_GXSetTevIndirect(ppc::Ctx& c) {
    matrix_boundary(0x802872A4);
    const auto stage=c.r[3],utc_lod=ppc::rd8(c,c.r[1]+11),alpha=read32(c.r[1]+12); // 9th/10th arguments on the caller's stack
    if(stage>15) throw gx::Error(gx::ErrorCode::layout,0x802872A4,15,stage);
    native_bp((0x10+stage)<<24|(c.r[4]&3)|(c.r[5]&3)<<2|(c.r[6]&7)<<4|(alpha&3)<<7|(c.r[7]&15)<<9|(c.r[8]&7)<<13|(c.r[9]&7)<<16|(utc_lod&1u)<<19|(c.r[10]&1)<<20);
    ppc::wr16(c,read32(c.r[2]-8968)+2,0);
}
void hle_GXSetIndTexMtx(ppc::Ctx& c) {
    matrix_boundary(0x8028730C);
    const auto id=c.r[3];
    const uint32_t index=(id>=1 && id<=3) ? id-1 : (id>=5 && id<=7) ? id-5 : (id>=9 && id<=11) ? id-9 : 0;
    memory.read(c.r[4],24);
    // offset[2][3] row-major; the SDK scales by 1024.0f (SDA2 constant) and truncates like fctiwz.
    auto fixed=[&](uint32_t i) {
        const float scaled=std::bit_cast<float>(read32(c.r[4]+i*4))*1024.0f;
        const float clamped=std::isnan(scaled) ? 0.f : std::clamp(scaled,-2147483648.f,2147483520.f);
        return uint32_t(int32_t(clamped))&0x7FF;
    };
    const uint32_t exponent=uint32_t(int32_t(c.r[5])+17);
    for(uint32_t column=0;column<3;++column)
        native_bp((index*3+6+column)<<24|fixed(column)|fixed(3+column)<<11|((exponent>>(column*2))&3)<<22);
    ppc::wr16(c,read32(c.r[2]-8968)+2,0);
}
void hle_GXSetIndTexCoordScale(ppc::Ctx& c) {
    matrix_boundary(0x80287460);
    const auto stage=c.r[3],sdk=read32(c.r[2]-8968);
    if(stage<4) {
        const uint32_t at=sdk+(stage<2 ? 0x178 : 0x17C),shift=(stage&1)*8;
        const auto word=(read32(at)&~(0xFFu<<shift)&0xFFFFFF)|(c.r[4]&15)<<shift|(c.r[5]&15)<<(shift+4)|(stage<2 ? 0x25u : 0x26u)<<24;
        ppc::wr32(c,at,word);
        native_bp(word);
    }
    ppc::wr16(c,sdk+2,0);
}
void hle_GXSetIndTexOrder(ppc::Ctx& c) {
    matrix_boundary(0x80287564);
    const auto stage=c.r[3],sdk=read32(c.r[2]-8968);
    const uint32_t coord=c.r[4]==255 ? 0 : c.r[4],map=c.r[5]==255 ? 0 : c.r[5];
    auto word=read32(sdk+0x170);
    if(stage<4) word=(word&~(0x3Fu<<(stage*6)))|(map&7)<<(stage*6)|(coord&7)<<(stage*6+3);
    ppc::wr32(c,sdk+0x170,word);
    native_bp(word);
    ppc::wr32(c,sdk+0x5FC,read32(sdk+0x5FC)|3);
    ppc::wr16(c,sdk+2,0);
}
namespace {
uint32_t fog_float(float value) { const auto bits=std::bit_cast<uint32_t>(value); return ((bits>>12)&0x7FF)|((bits>>23)&0xFF)<<11|(bits>>31)<<19; }
}
// GXSetFog (80287DC4): single-precision steps and constants as in the SDK body (0, 0.5, 1, 2, 8388638 from SDA2).
void hle_GXSetFog(ppc::Ctx& c) {
    matrix_boundary(0x80287DC4);
    const auto type=c.r[3];
    const float start=float(c.f[1]),end=float(c.f[2]),nearz=float(c.f[3]),farz=float(c.f[4]);
    const auto color=read32(c.r[4]);
    float a=0,cvalue=0; uint32_t b_word=0,e_word=0; // ortho leaves both B words zero
    if(type&8) {
        if(farz!=nearz && end!=start) {
            const float scale=1.0f/(end-start);
            a=scale*(farz-nearz); cvalue=scale*(start-nearz);
        }
    } else {
        float b=0.5f,a_num=0;
        if(farz!=nearz && end!=start) {
            const float depth=farz-nearz,range=end-start;
            a_num=farz*nearz; b=farz/depth; a_num=a_num/(depth*range); cvalue=start/range;
        }
        int exponent=0;
        while(double(b)>1.0) { b*=0.5f; ++exponent; }
        while(b>0.0f && double(b)<0.5) { b*=2.0f; --exponent; }
        a=a_num/float(int32_t(1u<<(exponent+1)));
        const float mantissa=8388638.0f*b;
        b_word=0xEF000000|(uint32_t(std::max(mantissa,0.f))&0xFFFFFF);
        e_word=0xF0000000|(uint32_t(exponent+1)&31);
    }
    native_bp(0xEE000000|fog_float(a));
    native_bp(b_word); native_bp(e_word);
    native_bp(0xF1000000|fog_float(cvalue)|((type>>3)&1)<<20|(type&7)<<21);
    native_bp(0xF2000000|color>>8);
    ppc::wr16(c,read32(c.r[2]-8968)+2,0);
}
void hle_ResChannelApply(ppc::Ctx& c) {
    matrix_boundary(0x80376148);
    if(recording) throw std::invalid_argument("recorded native channel resource requires implementation");
    const auto data=read32(c.r[3]),sdk=read32(c.r[2]-8968);
    std::vector<std::pair<uint32_t,uint32_t>> updates;
    if(data) {
        memory.read(data,40); memory.read(sdk,0x600); memory.read(c.r[8],4);
        const auto count=ppc::rd8(c,0x806A0041);
        if(count>2) throw std::invalid_argument("invalid native material channel count");
        for(uint32_t channel=0;channel<count;++channel) {
            const auto at=data+channel*20,flags=read32(at);
            if(flags&3) updates.emplace_back(sdk+0xB0+channel*4,read32(at+4));
            if(flags&12) {
                auto ambient=read32(at+8);
                if(!channel) {
                    const auto multiplier=read32(c.r[8]); uint32_t value=0;
                    for(uint32_t lane=0;lane<4;++lane) {
                        const auto shift=24-lane*8;
                        value|=((((ambient>>shift)&255)*((multiplier>>shift)&255)+127)/255)<<shift;
                    }
                    ambient=value;
                }
                updates.emplace_back(sdk+0xA8+channel*4,ambient);
            }
            for(uint32_t alpha=0;alpha<2;++alpha) if(flags&(16u<<alpha)) {
                const auto mask=c.r[4+channel*2+alpha];
                if(mask>255) throw std::invalid_argument("invalid native material light mask");
                const auto control=(read32(at+12+alpha*4)&(c.r[9] ? 0xFFFF87C1u : 0xFFFF87C3u))|((mask&15)<<2)|((mask&240)<<7);
                updates.emplace_back(sdk+0xB8+channel*4+alpha*8,control);
            }
        }
        if(count<2) { updates.emplace_back(sdk+0xBC,0); updates.emplace_back(sdk+0xC4,0); }
        updates.emplace_back(sdk+0x254,(read32(sdk+0x254)&~0x70u)|(uint32_t(count)<<4));
    }
    if(profile_mesh && channel_profile_count<32) {
        ++channel_profile_count;
        const auto data=read32(c.r[3]);
        if(data) {
            memory.read(data,40); memory.read(c.r[8],4);
            std::fprintf(stderr,"[native-channel] data %08X masks %u:%u ambient %08X disable %u count %u words",data,c.r[4],c.r[5],read32(c.r[8]),c.r[9],unsigned(ppc::rd8(c,0x806A0041)));
            for(uint32_t i=0;i<10;++i) std::fprintf(stderr," %08X",read32(data+i*4));
            std::fprintf(stderr,"\n");
        }
    }
    if(data) {
        const auto mode=0x806A0040u,flags=read32(mode+8);
        if((flags&4) && (flags&3)!=3) {
            const auto tex=ppc::rd8(c,mode),channels=ppc::rd8(c,mode+1),tev=ppc::rd8(c,mode+2),indirect=ppc::rd8(c,mode+3),cull=read32(mode+4);
            if(tex>8 || channels>2 || !tev || tev>16 || indirect>4 || cull>3) throw std::invalid_argument("invalid native material generation mode");
            memory.read(c.r[2]-5488+cull,1);
            const auto word=uint32_t(tex)|(uint32_t(channels)<<4)|(uint32_t(tev-1)<<10)|(uint32_t(indirect)<<16)|(uint32_t(ppc::rd8(c,c.r[2]-5488+cull))<<14);
            updates.emplace_back(sdk+0x254,(read32(sdk+0x254)&~0x7FC3Fu)|(word&0x7FC3Fu));
            updates.emplace_back(mode+8,flags|3);
        }
    }
    for(const auto& [address,value]:updates) ppc::wr32(c,address,value);
}
void hle_ResGenerationMode(ppc::Ctx& c) {
    matrix_boundary(0x80373D00);
    const auto tex=c.r[3],channels=c.r[4],tev=c.r[5],indirect=c.r[6],cull=c.r[7],sdk=read32(c.r[2]-8968);
    if(tex>8 || channels>2 || !tev || tev>16 || indirect>4 || cull>3) throw std::invalid_argument("invalid native resource generation mode");
    memory.read(sdk,0x600); memory.read(c.r[2]-5488+cull,1);
    const auto native_cull=ppc::rd8(c,c.r[2]-5488+cull);
    if(native_cull>3) throw std::invalid_argument("invalid native cull conversion");
    const auto word=tex|(channels<<4)|((tev-1)<<10)|(indirect<<16)|(uint32_t(native_cull)<<14);
    ppc::wr32(c,sdk+0x254,(read32(sdk+0x254)&~0x7FC3Fu)|(word&0x7FC3Fu));
    if(recording && !recording->overflow) recording->native.geometry.push_back(Resource::NumChannels{channels});
}
void hle_GXSetNumChans(ppc::Ctx& c) {
    matrix_boundary(0x80286190);
    const auto count=c.r[3],sdk=read32(c.r[2]-8968);
    if(count>2) throw std::invalid_argument("invalid native color channel count");
    memory.read(sdk,0x600);
    ppc::wr32(c,sdk+0x254,(read32(sdk+0x254)&~0x70u)|(count<<4));
    ppc::wr32(c,sdk+0x5FC,read32(sdk+0x5FC)|0x01000004u);
    if(recording && !recording->overflow) recording->native.geometry.push_back(Resource::NumChannels{count});
}
void hle_GXSetChanCtrl(ppc::Ctx& c) {
    matrix_boundary(0x802861B4);
    const auto channel=c.r[3],enable=c.r[4],ambient=c.r[5],material=c.r[6],mask=c.r[7],diffuse=c.r[8],attenuation=c.r[9],sdk=read32(c.r[2]-8968);
    if(channel>5 || enable>1 || ambient>1 || material>1 || mask>255 || diffuse>2 || attenuation>2) throw std::invalid_argument("invalid native color channel control");
    memory.read(sdk,0x600);
    const uint32_t attenuation_modes[]={1,3,2};
    const auto control=material|(enable<<1)|((mask&15)<<2)|(ambient<<6)|((attenuation ? diffuse : 0)<<7)|(attenuation_modes[attenuation]<<9)|((mask&240)<<7);
    const auto publish=[&](uint32_t index) {
        ppc::wr32(c,sdk+0xB8+index*4,control);
        ppc::wr32(c,sdk+0x5FC,read32(sdk+0x5FC)|(0x1000u<<index));
        if(recording && !recording->overflow) recording->native.geometry.push_back(Resource::ChannelControl{index,control});
    };
    publish(channel&3);
    if(channel>=4) publish((channel&1)+2);
}
void hle_GXLoadLightObjImm(ppc::Ctx& c) {
    matrix_boundary(0x80285F64);
    const auto pointer=c.r[3],mask=c.r[4];
    if(!mask || mask>255) throw std::invalid_argument("invalid native light id");
    memory.read(pointer,64); memory.read(read32(c.r[2]-8968),4);
    const auto id=uint32_t(std::bit_width(mask)-1);
    gx::LightUniform light;
    for(uint32_t lane=0;lane<4;++lane) light.color[lane]=float(ppc::rd8(c,pointer+12+lane));
    std::array<float,4>* fields[]={&light.cosine,&light.distance,&light.position,&light.direction};
    for(uint32_t field=0;field<4;++field) for(uint32_t lane=0;lane<3;++lane) {
        const auto value=ppc::rdf32(c,pointer+16+field*12+lane*4);
        if(!std::isfinite(value)) throw std::invalid_argument("nonfinite native light");
        (*fields[field])[lane]=value;
    }
    color_uniforms.lights[id]=light;
    if(recording) {
        uc::gx_fifo_write(1,0x10); uc::gx_fifo_write(4,0x000F0600|id*16);
        for(uint32_t i=0;i<3;++i) uc::gx_fifo_write(4,0);
        for(uint32_t i=3;i<16;++i) uc::gx_fifo_write(4,read32(pointer+i*4));
        if(!recording->overflow) recording->native.geometry.push_back(Resource::Light{id,light});
    }
    ppc::wr16(c,read32(c.r[2]-8968)+2,1);
}
void hle_GXLoadNrmMtxImm(ppc::Ctx& c) {
    matrix_boundary(0x802888A8);
    const auto pointer=c.r[3],row=c.r[4]; memory.read(pointer,44);
    gx::NormalMatrix normal{};
    for(uint32_t i=0;i<9;++i) normal[i/3][i%3]=ppc::rdf32(c,pointer+(i/3)*16+(i%3)*4);
    auto checked=transforms; gx::set_normal_matrix(checked,row,normal);
    if(recording) {
        uc::gx_fifo_write(1,0x10); uc::gx_fifo_write(4,0x00080400|row*3);
        for(const auto& values:normal) for(auto value:values) uc::gx_fifo_write(4,std::bit_cast<uint32_t>(value));
        if(!recording->overflow) recording->native.geometry.push_back(Resource::Normal{row,normal});
    } else transforms=checked;
}
void hle_GXLoadTexMtxImm(ppc::Ctx& c) {
    matrix_boundary(0x80288954);
    const auto pointer=c.r[3],id=c.r[4],type=c.r[5];
    if(type>1 || id>125) throw gx::Error(gx::ErrorCode::layout,pointer,125,id);
    const auto count=type ? 8u : 12u;
    memory.read(pointer,count*4);
    TextureMatrix matrix{{{},{},{0,0,1,0}}}; // a 2x4 load leaves Q to the identity row
    for(size_t i=0;i<count;++i) { const auto value=std::bit_cast<float>(read32(pointer+uint32_t(i*4))); if(!std::isfinite(value)) throw gx::Error(gx::ErrorCode::layout,pointer,0,i); matrix[i/4][i%4]=value; }
    texture_matrices[id]=matrix;
    const auto address=id>=64 ? 0x500+(id-64)*4 : id*4;
    uc::gx_fifo_write(1,0x10); uc::gx_fifo_write(4,(count-1)<<16|address);
    for(uint32_t i=0;i<count;++i) uc::gx_fifo_write(4,read32(pointer+i*4));
    if(recording && !recording->overflow) recording->native.geometry.push_back(Resource::TextureTransform{id,matrix});
}
