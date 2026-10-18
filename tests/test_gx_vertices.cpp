#include "check.h"
#include "gx_vertices.h"
#include <algorithm>
#include <cstring>

static std::span<const std::byte> bytes(const std::vector<uint8_t>& v) {
    return {reinterpret_cast<const std::byte*>(v.data()), v.size()};
}
template<class Fn> static void rejects(gx::ErrorCode code, Fn fn) {
    bool caught = false;
    try { fn(); } catch (const gx::Error& e) { caught = true; CHECK(e.code == code); }
    CHECK(caught);
}
int main() {
    std::vector<std::byte> ram(256);
    gx::Vertices capture({ram, {}, {}});
    gx::State state;
    state.descriptors[9] = gx::Mode::direct;
    state.descriptors[11] = gx::Mode::direct;
    state.descriptors[13] = gx::Mode::direct;
    state.formats[0][13].components = 2;
    const std::vector<uint8_t> triangle{
        0,0,0,0, 0,0,0,0, 0,0,0,0, 255,0,0,255, 0,0,0,0, 0,0,0,0,
        0x3F,0x80,0,0, 0,0,0,0, 0,0,0,0, 0,255,0,255, 0x3F,0x80,0,0, 0,0,0,0,
        0,0,0,0, 0x3F,0x80,0,0, 0,0,0,0, 0,0,255,255, 0,0,0,0, 0x3F,0x80,0,0
    };
    capture.begin(state, 0x90, 0, 3, 7, 0x80029D84);
    auto input = bytes(triangle);
    capture.write_attribute(7, input.first(1));
    capture.write_attribute(7, input.subspan(1, 7));
    capture.write_attribute(7, input.subspan(8));
    CHECK(!capture.active());
    state.formats[0][9].components = 2;
    auto draw = capture.take_draw();
    CHECK(draw.vertices.size() == 3 && draw.indices == std::vector<uint32_t>({0,1,2}));
    CHECK(draw.state->formats[0][9].components == 3 && draw.callsite == 0x80029D84);
    CHECK(draw.vertices[1].position[0] == 1 && draw.vertices[2].position[1] == 1);
    CHECK((draw.vertices[0].color[0] == std::array<uint8_t,4>({255,0,0,255})));
    CHECK(draw.vertices[2].texcoord[0][1] == 1);
    // Every split point exercises partial assembly followed by complete vertices.
    for (size_t split = 0; split < input.size(); ++split) {
        gx::Vertices divided({ram, {}, {}});
        divided.begin(*draw.state, 0x90, 0, 3, 7, 0x80029D84);
        divided.write_attribute(7, input.first(split));
        divided.write_attribute(7, input.subspan(split));
        auto result = divided.take_draw();
        CHECK(result.indices == draw.indices && result.vertices.size() == draw.vertices.size());
        for (size_t i = 0; i < result.vertices.size(); ++i) {
            CHECK(result.vertices[i].position == draw.vertices[i].position);
            CHECK(result.vertices[i].color == draw.vertices[i].color);
            CHECK(result.vertices[i].texcoord == draw.vertices[i].texcoord);
        }
    }
    rejects(gx::ErrorCode::sequence, [&] { capture.write_attribute(7, input.first(1)); });
    // A different VAT and indexed quantized positions read validated guest arrays.
    state.descriptors[11] = state.descriptors[13] = gx::Mode::none;
    state.descriptors[0] = gx::Mode::direct;
    state.descriptors[9] = gx::Mode::index16;
    state.formats[1][9] = {gx::Type::s16, 3, 1};
    state.arrays[0] = {0x80000020, 8};
    const std::vector<uint8_t> array_data{0xFF,0xFE, 0,4, 0,6, 0,0, 0,8, 0,10, 0,12};
    std::memcpy(ram.data() + 32, array_data.data(), array_data.size());
    capture.begin(state, 0xA8, 1, 2, 8, 0x80010000);
    const std::vector<uint8_t> indexed{3,0,0, 6,0,1};
    capture.write_attribute(8, bytes(indexed));
    draw = capture.take_draw();
    std::fill(ram.begin(), ram.end(), std::byte{0});
    CHECK((draw.vertices[0].position == std::array<float,3>({-1,2,3})));
    CHECK((draw.vertices[1].position == std::array<float,3>({4,5,6})));
    CHECK(draw.vertices[0].position_matrix == 3 && draw.vertices[1].position_matrix == 6);
    CHECK(draw.indices == std::vector<uint32_t>({0,1}));
    // Per-vertex texture matrix ids (TexNMtxIdx) follow the position matrix index; absent ones stay 255.
    state.descriptors[1] = state.descriptors[3] = gx::Mode::direct;
    CHECK(gx::has_texture_matrix_index(state));
    capture.begin(state, 0xA8, 1, 2, 8, 0x80010000);
    const std::vector<uint8_t> with_ids{3,30,33,0,0, 6,57,60,0,1};
    capture.write_attribute(8, bytes(with_ids));
    draw = capture.take_draw();
    CHECK((draw.vertices[0].texture_matrix == std::array<uint8_t,8>({30,255,33,255,255,255,255,255})));
    CHECK((draw.vertices[1].texture_matrix == std::array<uint8_t,8>({57,255,60,255,255,255,255,255})));
    CHECK(draw.vertices[1].position_matrix == 6);
    state.descriptors[1] = state.descriptors[3] = gx::Mode::none;
    CHECK(!gx::has_texture_matrix_index(state));
    // Winding differs on alternating strip triangles; quads keep SDK order.
    state.descriptors[0] = gx::Mode::none; state.descriptors[9] = gx::Mode::direct;
    state.formats[0][9] = {gx::Type::u8, 2, 0};
    const std::vector<uint8_t> quad{0,0, 1,0, 1,1, 0,1};
    for (auto primitive : {0x80, 0x98, 0xA0}) {
        capture.begin(state, uint8_t(primitive), 0, 4, 7, 0x80020000);
        capture.write_attribute(7, bytes(quad)); draw = capture.take_draw();
        const std::vector<uint32_t> want = primitive == 0x98 ? std::vector<uint32_t>{0,1,2, 2,1,3} : std::vector<uint32_t>{0,1,2, 0,2,3};
        CHECK(draw.indices == want);
    }
    capture.begin(state, 0xA8, 0, 4, 7, 0x80020004);
    capture.write_attribute(7, bytes(quad)); draw = capture.take_draw();
    CHECK(draw.indices == std::vector<uint32_t>({0,1,2,3}));
    capture.begin(state, 0xB0, 0, 3, 7, 0x80020008);
    capture.write_attribute(7, bytes(quad).first(6)); draw = capture.take_draw();
    CHECK(draw.indices == std::vector<uint32_t>({0,1,1,2}));
    rejects(gx::ErrorCode::primitive, [&] { capture.begin(state, 0xA8, 0, 3, 7, 0); });
    capture.begin(state, 0x90, 0, 3, 7, 0x80030000);
    capture.write_attribute(7, bytes(quad).first(2));
    rejects(gx::ErrorCode::producer, [&] { capture.write_attribute(8, bytes(quad).first(2)); });
    rejects(gx::ErrorCode::sequence, [&] { capture.begin(state, 0x90, 0, 3, 8, 0); });
    rejects(gx::ErrorCode::truncated, [&] { capture.finish(); });
    capture.write_attribute(7, bytes(quad).first(4)); draw = capture.take_draw();
    CHECK(draw.vertices.size() == 3);
    rejects(gx::ErrorCode::layout, [&] { capture.begin(state, 0x90, 8, 3, 7, 0); });
    rejects(gx::ErrorCode::primitive, [&] { capture.begin(state, 0x80, 0, 3, 7, 0); });
    state.formats[0][9].components = 9;
    rejects(gx::ErrorCode::layout, [&] { capture.begin(state, 0x90, 0, 3, 7, 0); });
    state.formats[0][9] = {gx::Type::u8, 2, 0};
    state.descriptors[9] = gx::Mode::index8; state.arrays[0] = {0x800000FF, 2};
    gx::Vertices bad_array({ram, {}, {}});
    bad_array.begin(state, 0xB8, 0, 1, 7, 0x80040000);
    const std::vector<uint8_t> index{0};
    rejects(gx::ErrorCode::array_bounds, [&] { bad_array.write_attribute(7, bytes(index)); });
    state.arrays[0] = {0xCC008000, 2};
    gx::Vertices mmio_array({ram, {}, {}});
    mmio_array.begin(state, 0xB8, 0, 1, 7, 0x80040004);
    rejects(gx::ErrorCode::array_bounds, [&] { mmio_array.write_attribute(7, bytes(index)); });
    // Indexed source collection spans every referenced array element.
    state={}; state.descriptors[9]=gx::Mode::index8;
    state.formats[0][9]={gx::Type::u8,2,0}; state.arrays[0]={0x80000020,4};
    gx::Vertices collected({ram,{},{}},true);
    collected.begin(state,0x90,0,3,7,0);
    const std::vector<uint8_t> source_indices{2,0,1};
    collected.write_attribute(7,bytes(source_indices)); collected.take_draw();
    CHECK(collected.sources()[0] && collected.sources()[0]->address==0x80000020 && collected.sources()[0]->size==10);
    CHECK(!collected.sources()[1]);
    collected.begin(state,0xB8,0,1,7,0);
    collected.write_attribute(7,bytes(source_indices).first(1)); collected.take_draw();
    CHECK(collected.sources()[0]->address==0x80000028 && collected.sources()[0]->size==2);
    return 0;
}
