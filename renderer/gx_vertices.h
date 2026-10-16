#pragma once
#include "gx_shared.h"
#include "gx_transforms.h"
#include "gx_textures.h"
#include "gx_materials.h"
#include "prompt_draw.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace gx {
enum class Mode : uint8_t { none, direct, index8, index16 };
enum class Type : uint8_t { u8, s8, u16, s16, f32 };
enum class Color : uint8_t { rgb565, rgb8, rgbx8, rgba4, rgba6, rgba8 };
enum class ErrorCode { layout, primitive, sequence, truncated, array_bounds, producer, canceled };
struct Error : std::runtime_error {
    Error(ErrorCode code, uint32_t address, size_t expected, size_t received);
    ErrorCode code;
    uint32_t address;
    size_t expected, received;
};
struct Format { Type type = Type::f32; uint8_t components = 3, fraction = 0; Color color = Color::rgba8; bool normal_index3 = false; bool operator==(const Format&) const = default; };
struct Array { uint32_t address = 0; uint8_t stride = 0; bool operator==(const Array&) const = default; };
struct State {
    uint8_t cull=0; // native material: none/back/front/all
    std::array<Mode, 21> descriptors{};
    std::array<std::array<Format, 21>, 8> formats{};
    std::array<Array, 16> arrays{}; // vertex attributes followed by indexed matrix/light arrays
    std::optional<std::array<float,6>> viewport;
    std::optional<std::array<int32_t,4>> scissor;
};
struct Memory {
    std::span<const std::byte> mem1, mem2, locked_cache;
    std::span<const std::byte> read(uint32_t address, size_t size) const;
};
size_t encoded_draw_size(const State& state, uint8_t primitive, uint8_t vat, uint16_t count, uint32_t callsite);
struct Vertex {
    std::array<float, 3> position{};
    std::array<float, 9> normal{};
    std::array<std::array<uint8_t, 4>, 2> color{{{255, 255, 255, 255}, {255, 255, 255, 255}}};
    std::array<std::array<float, 2>, 8> texcoord{};
    uint8_t position_matrix = 0;
    std::array<uint8_t, 8> texture_matrix{};
};
struct BlendState {
    uint8_t mode=0,source=1,destination=0,logic=3;
    bool color_write=true,alpha_write=true;
};
struct MeshBuffers { std::vector<Vertex> vertices; std::vector<uint32_t> indices; };   // immutable once cached; shared by every draw of the mesh
struct DrawPacket {
    uint32_t callsite = 0;
    uint8_t primitive = 0, vat = 0;
    Shared<State> state;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    Shared<Transforms> transforms;
    std::shared_ptr<const TexturePixels> texture;
    std::array<std::shared_ptr<const TexturePixels>,7> texture_maps;
    TevMaterial material;
    Shared<DrawUniform> uniforms;
    BlendState blend;
    std::array<uint8_t,8> texture_wrap{}; // SDK mode: S0..1,T2..3,mag4,mip5..6,min7
    std::optional<PromptDraw> native_prompt;
    bool late_aim = false;   // an aim reticle quad: the renderer moves it to the freshest pointer (late_aim.h)
    std::shared_ptr<const MeshBuffers> mesh;   // when set, the geometry lives here and `vertices`/`indices` stay empty
    std::span<const Vertex> vertex_view() const { return mesh ? std::span<const Vertex>(mesh->vertices) : std::span<const Vertex>(vertices); }
    std::span<const uint32_t> index_view() const { return mesh ? std::span<const uint32_t>(mesh->indices) : std::span<const uint32_t>(indices); }
};

// Owned by the guest scheduler baton; callers identify its current producer.
class Vertices {
public:
    struct SourceRange { uint32_t address; size_t size; };
    explicit Vertices(Memory memory, bool collect_sources=false) : memory_(memory), collect_sources_(collect_sources) {}
    void begin(const State& state, uint8_t primitive, uint8_t vat, uint16_t count, uint32_t producer, uint32_t callsite) { begin(Shared<State>(state), primitive, vat, count, producer, callsite); }
    void begin(const Shared<State>& state, uint8_t primitive, uint8_t vat, uint16_t count, uint32_t producer, uint32_t callsite);   // shares the block with the draw
    void write_attribute(uint32_t producer, std::span<const std::byte> bytes);
    DrawPacket take_draw();
    void finish() const;
    bool active() const { return active_; }
    const auto& sources() const { return sources_; }
private:
    Vertex decode(std::span<const std::byte> data);
    Memory memory_;
    DrawPacket draw_;
    std::vector<std::byte> vertex_bytes_;
    size_t vertex_size_ = 0;
    uint16_t count_ = 0;
    uint32_t producer_ = 0;
    bool active_ = false, ready_ = false;
    bool collect_sources_;
    std::array<std::optional<SourceRange>,12> sources_{};
};

} // namespace gx
