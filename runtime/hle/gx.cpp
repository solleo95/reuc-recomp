#include "gx.h"
#include "guest_call.h"
#include "hle/os.h"
#include "gx_audit.h"
#include <atomic>
#include <bit>
#include <cstring>
#include <map>
#include <variant>

namespace {
uc::Sched* scheduler;
gx::Packets* packets;
gx::Memory memory;
gx::State state;
gx::Transforms transforms,live_transforms;
gx::BlendState blend_state,live_blend;
struct TextureSource { uint32_t address; gx::TextureDesc desc; std::optional<uint32_t> palette; };
struct PaletteSource { uint32_t address, entries; gx::PaletteFormat format; };
struct PaletteData { gx::PaletteFormat format; std::vector<std::byte> bytes; };
using Palettes=std::map<uint32_t,std::shared_ptr<const PaletteData>>;
// shortcut: SDK region identity; unify aliased regions if observed (TD-038).
Palettes palettes;
using TextureBindings=std::array<std::optional<TextureSource>,8>;
TextureBindings bound_textures;
std::shared_ptr<const gx::TexturePixels> live_texture;
struct CopyState { std::array<uint8_t,4> color{0,0,0,255}; uint32_t depth=0xFFFFFF; };
CopyState copy_state;
std::unique_ptr<gx::Vertices> capture;
std::vector<gx::DrawPacket> draws;
struct RecordedDraw {
    gx::State state;
    uint32_t callsite;
    uint8_t primitive, vat;
    uint16_t count;
    size_t expected;
    std::vector<std::byte> payload;
};
struct Resource {
    std::vector<std::byte> bytes;
    struct ArrayBinding { size_t attribute; gx::Array array; };
    struct Child { uint32_t address, length; };
    struct Projection { gx::Projection matrix; };
    struct Position { uint32_t row; gx::PositionMatrix matrix; };
    struct IndexedPosition { uint16_t index; uint32_t row; };
    struct Current { uint32_t row; };
    struct CopyClear { CopyState state; };
    struct TextureBinding { uint32_t map; TextureSource source; };
    struct PaletteLoad { uint32_t region; PaletteSource source; };
    struct Blend { gx::BlendState state; };
    std::vector<std::variant<RecordedDraw,ArrayBinding,Child,Projection,Position,IndexedPosition,Current,CopyClear,TextureBinding,PaletteLoad,Blend>> geometry;
};
std::map<uint32_t,Resource> resources; // owned SDK recordings; no FIFO command decoding
struct ListRecording {
    uint32_t address, capacity, sdk, owner;
    bool restore, overflow = false;
    std::array<std::byte,0x600> shadow{};
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
bool recording_primitive() {
    if (!recording || recording->native.geometry.empty()) return false;
    auto* draw=std::get_if<RecordedDraw>(&recording->native.geometry.back());
    return draw && draw->payload.size()<draw->expected;
}
gx::State snapshot(ppc::Ctx& ctx) {
    uint32_t sdk = read32(ctx.r[2] - 8968);
    uint32_t low = read32(sdk + 0x14), high = read32(sdk + 0x18);
    gx::State result = recording ? recording->input_state : state;
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
    if (found==resources.end() || found->second.bytes.size()!=length || !std::equal(bytes.begin(),bytes.end(),found->second.bytes.begin()))
        throw gx::Error(gx::ErrorCode::layout,address,length,0); // TD-014: prebuilt/modified lists require native import
    return found->second;
}
std::shared_ptr<const PaletteData> load_palette(const PaletteSource& source) {
    if (!source.entries || source.entries>16384 || uint32_t(source.format)>2)
        throw gx::TextureError(gx::TextureErrorCode::descriptor,"invalid native palette descriptor");
    auto bytes=memory.read(source.address,source.entries*2);
    return std::make_shared<const PaletteData>(PaletteData{source.format,{bytes.begin(),bytes.end()}});
}
gx::TextureCache texture_cache;
std::shared_ptr<const gx::TexturePixels> snapshot_texture(const std::optional<TextureSource>& source,const Palettes& loaded) {
    if (!source) return {};
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
void import_resource(uint32_t address,uint32_t length,gx::State& bindings,gx::Transforms& transform_state,CopyState& copy,TextureBindings& textures,Palettes& loaded,gx::BlendState& blend,std::vector<gx::DrawPacket>& imported,std::vector<uint32_t>& path) {
    const uint32_t physical=address & 0x3FFFFFFF;
    // shortcut: nesting capped at64; replace traversal if a verified deeper native asset appears (TD-032).
    if (path.size()==64 || std::find(path.begin(),path.end(),physical)!=path.end())
        throw gx::Error(gx::ErrorCode::sequence,address,64,path.size());
    const auto& resource=checked_resource(address,length);
    path.push_back(physical);
    for (const auto& item : resource.geometry) {
        if (const auto* source=std::get_if<RecordedDraw>(&item)) {
            // shortcut: decode attributes per call; cache native mesh revisions after profiling (TD-032).
            auto layout=source->state;
            layout.arrays=bindings.arrays;
            gx::Vertices vertices(memory);
            vertices.begin(layout,source->primitive,source->vat,source->count,producer(),source->callsite);
            vertices.write_attribute(producer(),source->payload);
            auto draw=vertices.take_draw(); draw.transforms=transform_state;
            draw.texture=snapshot_texture(textures[0],loaded);
            draw.blend=blend;
            imported.push_back(std::move(draw));
        } else if (const auto* array=std::get_if<Resource::ArrayBinding>(&item)) bindings.arrays[array->attribute]=array->array;
        else if (const auto* projection=std::get_if<Resource::Projection>(&item)) transform_state.projection=projection->matrix;
        else if (const auto* matrix=std::get_if<Resource::Position>(&item)) gx::set_position_matrix(transform_state,matrix->row,matrix->matrix);
        else if (const auto* matrix=std::get_if<Resource::IndexedPosition>(&item)) indexed_position(transform_state,bindings,matrix->index,matrix->row);
        else if (const auto* current=std::get_if<Resource::Current>(&item)) transform_state.current=current->row;
        else if (const auto* clear=std::get_if<Resource::CopyClear>(&item)) copy=clear->state;
        else if (const auto* binding=std::get_if<Resource::TextureBinding>(&item)) textures[binding->map]=binding->source;
        else if (const auto* palette=std::get_if<Resource::PaletteLoad>(&item)) loaded[palette->region]=load_palette(palette->source);
        else if (const auto* value=std::get_if<Resource::Blend>(&item)) blend=value->state;
        else {
            const auto& child=std::get<Resource::Child>(item);
            import_resource(child.address,child.length,bindings,transform_state,copy,textures,loaded,blend,imported,path);
        }
    }
    path.pop_back();
}
uint64_t submit_batch(bool present,bool clear,uint32_t xfb=0) {
    if (!packets) return 0;
    try {
        const uint64_t serial=packets->seal_batch({0,std::move(draws),present,clear,copy_state.color,copy_state.depth,xfb});
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
void gx_hle_init(Sched& sched, uint8_t* base, gx::Packets* queue) {
    scheduler = &sched; packets = queue;
    memory = {{reinterpret_cast<const std::byte*>(base), 24u << 20}, {reinterpret_cast<const std::byte*>(base + 0x10000000), 64u << 20}, {reinterpret_cast<const std::byte*>(base + 0x20000000), 0x4000}};
    state = {}; transforms={}; live_transforms={}; draws.clear(); finishes.clear(); resources.clear(); recording.reset(); capture = std::make_unique<gx::Vertices>(memory);
    copy_state={}; copies=0;
    texture_cache.clear(); texture_decodes=0;
    blend_state={}; live_blend={};
    bound_textures={}; palettes.clear(); live_texture.reset();
    draw_count = 0; vertex_count = 0; submitted = 0; completed = 0; outside_bytes = 0;
}
void gx_fifo_write(int size, uint32_t value) {
    if (size != 1 && size != 2 && size != 4) throw std::invalid_argument("invalid native GX write width");
    if (recording) {
        if (producer() != recording->owner) throw gx::Error(gx::ErrorCode::producer,recording->address,recording->owner,producer());
        if (recording->overflow) return; // GXEndDisplayList reports this failed recording with length zero
        if (size_t(size) > recording->capacity - recording->bytes.size()) { recording->overflow=true; return; }
        if (recording_primitive()) {
            auto& draw=std::get<RecordedDraw>(recording->native.geometry.back());
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
        draw.blend=live_blend;
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
GxStats gx_stats() { return {draw_count.load(), vertex_count.load(), submitted.load(), completed.load(), outside_bytes.load(),copies.load(),texture_decodes.load()}; }
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
            recording->native.geometry.push_back(RecordedDraw{layout,callsite,uint8_t(primitive),uint8_t(vat),uint16_t(count),expected,{}});
        }
        return;
    }
    auto texture=snapshot_texture(bound_textures[0],palettes);
    capture->begin(snapshot(c), uint8_t(c.r[3]), uint8_t(c.r[4]), uint16_t(c.r[5]), producer(), c.lr ? c.lr - 4 : 0x80284FD0);
    live_texture=std::move(texture);
    live_transforms=transforms;
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
    }
    list->bytes.reserve(std::min<size_t>(capacity,1024));
    ppc::wr8(c,sdk+0x5F8,1);
    recording=std::move(list);
}
void hle_GXEndDisplayList(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802885D8,c);
    if (!recording) throw gx::Error(gx::ErrorCode::sequence,0x802885D8,1,0);
    if (recording->owner!=producer()) throw gx::Error(gx::ErrorCode::producer,recording->address,recording->owner,producer());
    if (!recording->overflow && recording_primitive()) {
        const auto& draw=std::get<RecordedDraw>(recording->native.geometry.back());
        throw gx::Error(gx::ErrorCode::truncated,draw.callsite,draw.expected,draw.payload.size());
    }
    uc::call_guest(c,0x80284504,{}); // verified GXFlush: dirty state followed by 32 NOP bytes
    auto list=std::move(recording);
    const uint32_t length=list->overflow ? 0 : uint32_t(list->bytes.size()) & ~31u;
    if (length) std::memcpy(c.mem+(list->address & 0x3FFFFFFF),list->bytes.data(),length);
    if (length) {
        const uint32_t address=list->address & 0x3FFFFFFF;
        for (auto it=resources.begin();it!=resources.end();) {
            if (uint64_t(it->first)<uint64_t(address)+length && uint64_t(address)<uint64_t(it->first)+it->second.bytes.size()) it=resources.erase(it);
            else ++it;
        }
        list->native.bytes.assign(list->bytes.begin(),list->bytes.begin()+length);
        resources.emplace(address,std::move(list->native));
    }
    uc::gx_audit_recorded_resource(list->address,std::span<const std::byte>(list->bytes).first(length));
    if (list->restore) {
        const uint32_t kept=read32(list->sdk+8);
        std::memcpy(c.mem+(list->sdk & 0x3FFFFFFF),list->shadow.data(),list->shadow.size());
        ppc::wr32(c,list->sdk+8,kept);
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
    std::vector<gx::DrawPacket> imported;
    auto bindings=state;
    auto transform_state=transforms;
    auto copy=copy_state;
    auto textures=bound_textures;
    auto loaded=palettes;
    auto blend=blend_state;
    std::vector<uint32_t> path;
    import_resource(address,length,bindings,transform_state,copy,textures,loaded,blend,imported,path);
    for (auto& draw : imported) { vertex_count+=draw.vertices.size(); ++draw_count; draws.push_back(std::move(draw)); }
    state.arrays=bindings.arrays;
    transforms=transform_state;
    copy_state=copy;
    bound_textures=textures;
    palettes=std::move(loaded);
    blend_state=blend;
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
    if (format>255 || (format&15)!=((image0>>20)&15)) throw gx::TextureError(gx::TextureErrorCode::descriptor,"inconsistent native GXTexObj format");
    memory.read(source.address,gx::encoded_texture_size(source.desc));
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
    submit_batch(true,clear,xfb); ++copies;
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
