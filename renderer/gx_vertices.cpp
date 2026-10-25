#include "gx_vertices.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace gx {
namespace {
std::string describe(ErrorCode code, uint32_t address, size_t expected, size_t received) {
    char text[160];
    std::snprintf(text, sizeof(text), "GX error %u at %08X: expected %zu, received %zu", unsigned(code), address, expected, received);
    return text;
}
size_t scalar_size(Type type) {
    switch (type) {
    case Type::u8: case Type::s8: return 1;
    case Type::u16: case Type::s16: return 2;
    case Type::f32: return 4;
    }
    return 0;
}
size_t color_size(Color color) {
    switch (color) {
    case Color::rgb565: case Color::rgba4: return 2;
    case Color::rgb8: case Color::rgba6: return 3;
    case Color::rgbx8: case Color::rgba8: return 4;
    }
    return 0;
}
size_t data_size(size_t attr, const Format& fmt) {
    if (attr < 9) return 1;
    if (attr == 11 || attr == 12) return color_size(fmt.color);
    return scalar_size(fmt.type) * fmt.components;
}
uint32_t be(std::span<const std::byte> data) {
    uint32_t value = 0;
    for (auto b : data) value = (value << 8) | std::to_integer<uint8_t>(b);
    return value;
}
float scalar(std::span<const std::byte> data, Type type, uint8_t fraction) {
    uint32_t value = be(data);
    switch (type) {
    case Type::u8: case Type::u16: return std::ldexp(float(value), -int(fraction));
    case Type::s8: return std::ldexp(float(std::bit_cast<int8_t>(uint8_t(value))), -int(fraction));
    case Type::s16: return std::ldexp(float(std::bit_cast<int16_t>(uint16_t(value))), -int(fraction));
    case Type::f32: return std::bit_cast<float>(value);
    }
    return 0;
}
uint8_t expand(uint32_t value, unsigned bits) {
    return uint8_t((value << (8 - bits)) | (value >> (2 * bits - 8)));
}
std::array<uint8_t, 4> color(std::span<const std::byte> data, Color fmt) {
    uint32_t v = be(data);
    switch (fmt) {
    case Color::rgb565: return {expand((v >> 11) & 31, 5), expand((v >> 5) & 63, 6), expand(v & 31, 5), 255};
    case Color::rgb8: return {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v), 255};
    case Color::rgbx8: return {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), 255};
    case Color::rgba4: return {expand(v >> 12, 4), expand((v >> 8) & 15, 4), expand((v >> 4) & 15, 4), expand(v & 15, 4)};
    case Color::rgba6: return {expand((v >> 18) & 63, 6), expand((v >> 12) & 63, 6), expand((v >> 6) & 63, 6), expand(v & 63, 6)};
    case Color::rgba8: return {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    }
    return {};
}
void indices(DrawPacket& draw) {
    uint32_t n = uint32_t(draw.vertices.size());
    const size_t count=draw.primitive==0x80 ? size_t(n)/4*6 :
        (draw.primitive==0x98 || draw.primitive==0xA0) ? size_t(n>2 ? n-2 : 0)*3 :
        draw.primitive==0xB0 ? size_t(n ? n-1 : 0)*2 : n;
    draw.indices.reserve(count);
    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) { draw.indices.insert(draw.indices.end(), {a, b, c}); };
    switch (draw.primitive) {
    case 0x80:
        for (uint32_t i = 0; i < n; i += 4) { tri(i, i + 1, i + 2); tri(i, i + 2, i + 3); }
        break;
    case 0x98:
        for (uint32_t i = 2; i < n; ++i) { if (i & 1) tri(i - 1, i - 2, i); else tri(i - 2, i - 1, i); }
        break;
    case 0xA0: for (uint32_t i = 2; i < n; ++i) tri(0, i - 1, i); break;
    case 0xB0: for (uint32_t i = 1; i < n; ++i) draw.indices.insert(draw.indices.end(), {i - 1, i}); break;
    default: for (uint32_t i = 0; i < n; ++i) draw.indices.push_back(i); break;
    }
}
}

Error::Error(ErrorCode code, uint32_t address, size_t expected, size_t received)
    : std::runtime_error(describe(code, address, expected, received)), code(code), address(address), expected(expected), received(received) {}

std::span<const std::byte> Memory::read(uint32_t address, size_t size) const {
    uint32_t alias = address >> 28;
    if (alias != 0 && alias != 1 && alias != 8 && alias != 9 && alias != 12 && alias != 13 && alias != 14)
        throw Error(ErrorCode::array_bounds, address, size, 0);
    uint32_t physical = address & 0x3FFFFFFF;
    std::span<const std::byte> region;
    uint32_t base = 0;
    if (physical < 0x10000000) region = mem1;
    else if (physical < 0x20000000) { region = mem2; base = 0x10000000; }
    else if (physical < 0x20004000) { region = locked_cache; base = 0x20000000; }
    size_t offset = physical - base;
    if (offset > region.size() || size > region.size() - offset)
        throw Error(ErrorCode::array_bounds, address, size, offset > region.size() ? 0 : region.size() - offset);
    return region.subspan(offset, size);
}

static size_t encoded_vertex_size(const State& state, uint8_t vat, uint32_t callsite) {
    if (vat >= 8) throw Error(ErrorCode::layout, callsite, 8, vat);
    if (state.descriptors[9] == Mode::none) throw Error(ErrorCode::layout, callsite, 9, 0);
    size_t vertex_size = 0;
    for (size_t a = 0; a < state.descriptors.size(); ++a) {
        Mode mode = state.descriptors[a];
        if (mode == Mode::none) continue;
        const Format& fmt = state.formats[vat][a];
        bool format_ok = true;
        if (a < 9) format_ok = mode == Mode::direct;
        else if (a == 11 || a == 12) format_ok = color_size(fmt.color) != 0;
        else {
            format_ok = scalar_size(fmt.type) != 0 && fmt.fraction <= 31;
            if (a == 9) format_ok &= fmt.components == 2 || fmt.components == 3;
            else if (a == 10) format_ok &= (fmt.components == 3 || fmt.components == 9) && (fmt.type == Type::s8 || fmt.type == Type::s16 || fmt.type == Type::f32);
            else format_ok &= fmt.components == 1 || fmt.components == 2;
        }
        // shortcut: NBT3 array offsets need DOL/resource evidence before enabling this format.
        if (fmt.normal_index3) format_ok = false;
        if (!format_ok) throw Error(ErrorCode::layout, callsite, a, fmt.components);
        switch (mode) {
        case Mode::direct: vertex_size += data_size(a, fmt); break;
        case Mode::index8: vertex_size += fmt.normal_index3 ? 3 : 1; break;
        case Mode::index16: vertex_size += fmt.normal_index3 ? 6 : 2; break;
        default: throw Error(ErrorCode::layout, callsite, a, unsigned(mode));
        }
    }
    return vertex_size;
}

size_t encoded_draw_size(const State& state,uint8_t primitive,uint8_t vat,uint16_t count,uint32_t callsite) {
    bool valid = false;
    switch (primitive) {
    case 0x80: valid = count >= 4 && count % 4 == 0; break;
    case 0x90: valid = count >= 3 && count % 3 == 0; break;
    case 0x98: case 0xA0: valid = count >= 3; break;
    case 0xA8: valid = count >= 2 && count % 2 == 0; break;
    case 0xB0: valid = count >= 2; break;
    case 0xB8: valid = count >= 1; break;
    }
    if (!valid) throw Error(ErrorCode::primitive, callsite, primitive, count);
    return encoded_vertex_size(state,vat,callsite)*count;
}

void Vertices::begin(const Shared<State>& state, uint8_t primitive, uint8_t vat, uint16_t count, uint32_t producer, uint32_t callsite, bool discard) {
    if (active_ || ready_) throw Error(ErrorCode::sequence, callsite, 0, count);
    const size_t vertex_size=encoded_draw_size(*state,primitive,vat,count,callsite)/count;
    draw_ = {callsite, primitive, vat, state, {}, {}};
    discard_=discard; decoded_=0;
    if(!discard_)draw_.vertices.reserve(count);
    vertex_size_ = vertex_size; count_ = count; producer_ = producer;
    vertex_bytes_.clear(); vertex_bytes_.reserve(vertex_size);
    sources_={};
    active_ = true;
}

void Vertices::decode(std::span<const std::byte> data) {
    Vertex vertex;
    size_t cursor = 0;
    for (size_t a = 0; a < draw_.state->descriptors.size(); ++a) {
        Mode mode = draw_.state->descriptors[a];
        if (mode == Mode::none) continue;
        const Format& fmt = draw_.state->formats[draw_.vat][a];
        size_t groups = fmt.normal_index3 ? 3 : 1;
        for (size_t group = 0; group < groups; ++group) {
            size_t size = data_size(a, fmt) / groups;
            std::span<const std::byte> attr;
            if (mode == Mode::direct) { attr = data.subspan(cursor, size); cursor += size; }
            else {
                size_t index_size = mode == Mode::index8 ? 1 : 2;
                uint32_t index = be(data.subspan(cursor, index_size)); cursor += index_size;
                const Array& array = draw_.state->arrays[a - 9];
                uint64_t address = uint64_t(array.address) + uint64_t(index) * array.stride;
                if (!array.address || !array.stride || address > std::numeric_limits<uint32_t>::max())
                    throw Error(ErrorCode::array_bounds, array.address, size, array.stride);
                attr = memory_.read(uint32_t(address), size);
                if(collect_sources_) {
                    auto& range=sources_[a-9];
                    const auto begin=range ? std::min(uint64_t(range->address),address) : address;
                    const auto end=range ? std::max(uint64_t(range->address)+range->size,address+size) : address+size;
                    range=SourceRange{uint32_t(begin),size_t(end-begin)};
                }
            }
            // Replay still validates every indexed address; decoded values only serve rendering.
            if(discard_)continue;
            if (a == 0) vertex.position_matrix = uint8_t(be(attr));
            else if (a < 9) vertex.texture_matrix[a - 1] = uint8_t(be(attr));
            else if (a == 11 || a == 12) vertex.color[a - 11] = color(attr, fmt.color);
            else {
                size_t width = scalar_size(fmt.type);
                uint8_t fraction = a == 10 ? (fmt.type == Type::s8 ? 6 : fmt.type == Type::s16 ? 14 : 0) : fmt.fraction;
                for (size_t component = 0; component < fmt.components / groups; ++component) {
                    float value = scalar(attr.subspan(component * width, width), fmt.type, fraction);
                    if (a == 9) vertex.position[component] = value;
                    else if (a == 10) vertex.normal[group * 3 + component] = value;
                    else vertex.texcoord[a - 13][component] = value;
                }
            }
        }
    }
    if(!discard_)draw_.vertices.push_back(vertex);
    ++decoded_;
}

void Vertices::write_attribute(uint32_t producer, std::span<const std::byte> bytes) {
    if (!active_) throw Error(ErrorCode::sequence, draw_.callsite, 0, bytes.size());
    if (producer != producer_) throw Error(ErrorCode::producer, draw_.callsite, producer_, producer);
    size_t remaining = (count_ - decoded_) * vertex_size_ - vertex_bytes_.size();
    if (bytes.size() > remaining) throw Error(ErrorCode::truncated, draw_.callsite, remaining, bytes.size());
    while (!bytes.empty()) {
        if (vertex_bytes_.empty() && bytes.size() >= vertex_size_) {
            decode(bytes.first(vertex_size_));
            bytes = bytes.subspan(vertex_size_);
            continue;
        }
        size_t n = std::min(bytes.size(), vertex_size_ - vertex_bytes_.size());
        const size_t old = vertex_bytes_.size();
        vertex_bytes_.resize(old + n);
        std::memcpy(vertex_bytes_.data() + old, bytes.data(), n);
        bytes = bytes.subspan(n);
        if (vertex_bytes_.size() == vertex_size_) {
            decode(vertex_bytes_);
            vertex_bytes_.clear();
        }
    }
    if (decoded_ == count_) { if(!discard_)indices(draw_); active_ = false; ready_ = true; }
}

DrawPacket Vertices::take_draw() {
    if (!ready_) { finish(); throw Error(ErrorCode::sequence, draw_.callsite, count_, decoded_); }
    ready_ = false;
    return std::move(draw_);
}
void Vertices::finish() const {
    if (active_) throw Error(ErrorCode::truncated, draw_.callsite, count_ * vertex_size_, decoded_ * vertex_size_ + vertex_bytes_.size());
}

} // namespace gx
