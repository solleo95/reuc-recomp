#include "check.h"
#include "gx_textures.h"
#include <algorithm>
#include <array>
#include <limits>

using Pixel = std::array<uint8_t, 4>;
static std::span<const std::byte> bytes(const std::vector<uint8_t>& v) {
    return {reinterpret_cast<const std::byte*>(v.data()), v.size()};
}
static Pixel pixel(const gx::TexturePixels& image, size_t level, uint32_t x, uint32_t y) {
    const auto& mip = image.levels.at(level);
    CHECK(x < mip.width && y < mip.height);
    Pixel out;
    std::copy_n(mip.rgba.begin() + (size_t(y) * mip.width + x) * 4, 4, out.begin());
    return out;
}
template<class Fn> static void rejects(gx::TextureErrorCode code, Fn fn) {
    bool caught = false;
    try { fn(); } catch (const gx::TextureError& e) { caught = true; CHECK(e.code == code); }
    CHECK(caught);
}
int main() {
    CHECK(gx::encoded_texture_size({4,4,gx::TextureFormat::ia8})==32);
    CHECK(gx::encoded_texture_size({5,3,gx::TextureFormat::i8,3})==96);
    // Literal fixtures distinguish nibble order, row order and edge-tile padding.
    std::vector<uint8_t> data(64);
    data[0] = 0x1F; data[4] = 0xA2; data[32] = 0x70;
    auto image = gx::decode_texture({9,2,gx::TextureFormat::i4}, bytes(data));
    CHECK(pixel(image,0,0,0) == Pixel({17,17,17,17}));
    CHECK(pixel(image,0,1,0) == Pixel({255,255,255,255}));
    CHECK(pixel(image,0,0,1) == Pixel({170,170,170,170}));
    CHECK(pixel(image,0,8,0) == Pixel({119,119,119,119}));
    data.assign(128,0); data[0]=41; data[8]=53; data[32]=67; data[64]=79;
    image = gx::decode_texture({9,5,gx::TextureFormat::i8},bytes(data));
    CHECK(pixel(image,0,0,0) == Pixel({41,41,41,41}));
    CHECK(pixel(image,0,0,1) == Pixel({53,53,53,53}));
    CHECK(pixel(image,0,8,0) == Pixel({67,67,67,67}));
    CHECK(pixel(image,0,0,4) == Pixel({79,79,79,79}));
    data.assign(32,0); data[0]=0xA3;
    image = gx::decode_texture({1,1,gx::TextureFormat::ia4},bytes(data));
    CHECK(pixel(image,0,0,0) == Pixel({51,51,51,170}));
    data[0]=0x12; data[1]=0xC4;
    image = gx::decode_texture({1,1,gx::TextureFormat::ia8},bytes(data));
    CHECK(pixel(image,0,0,0) == Pixel({196,196,196,18}));
    data[0]=0xF8; data[1]=0; data[2]=0x07; data[3]=0xE0; data[4]=0; data[5]=0x1F;
    image = gx::decode_texture({3,1,gx::TextureFormat::rgb565},bytes(data));
    CHECK(pixel(image,0,0,0) == Pixel({255,0,0,255}));
    CHECK(pixel(image,0,1,0) == Pixel({0,255,0,255}));
    CHECK(pixel(image,0,2,0) == Pixel({0,0,255,255}));
    data[0]=0xFC; data[1]=0; data[2]=0x3A; data[3]=0xBC;
    image = gx::decode_texture({2,1,gx::TextureFormat::rgb5a3},bytes(data));
    CHECK(pixel(image,0,0,0) == Pixel({255,0,0,255}));
    CHECK(pixel(image,0,1,0) == Pixel({170,187,204,109}));
    data.assign(128,0); data[0]=17; data[1]=34; data[32]=51; data[33]=68;
    data[30]=85; data[31]=102; data[62]=119; data[63]=136;
    data[64]=153; data[65]=170; data[96]=187; data[97]=204;
    image = gx::decode_texture({5,4,gx::TextureFormat::rgba8},bytes(data));
    CHECK(pixel(image,0,0,0) == Pixel({34,51,68,17}));
    CHECK(pixel(image,0,3,3) == Pixel({102,119,136,85}));
    CHECK(pixel(image,0,4,0) == Pixel({170,187,204,153}));
    // Four CMPR sub-blocks, MSB-first selectors, Wii interpolation and transparent RGB.
    data.assign(32,0);
    data[0]=0xF8; data[1]=0; data[2]=0; data[3]=0x1F; data[4]=0x1B;
    data[8]=0x07; data[9]=0xE0; data[10]=0; data[11]=0;
    data[16]=0; data[17]=0x1F; data[18]=0; data[19]=0;
    data[24]=0; data[25]=0; data[26]=0xFF; data[27]=0xFF; data[28]=0xB0;
    image = gx::decode_texture({8,8,gx::TextureFormat::cmpr},bytes(data));
    CHECK(pixel(image,0,0,0) == Pixel({255,0,0,255}));
    CHECK(pixel(image,0,1,0) == Pixel({0,0,255,255}));
    CHECK(pixel(image,0,2,0) == Pixel({159,0,95,255}));
    CHECK(pixel(image,0,3,0) == Pixel({95,0,159,255}));
    CHECK(pixel(image,0,4,0) == Pixel({0,255,0,255}));
    CHECK(pixel(image,0,0,4) == Pixel({0,0,255,255}));
    CHECK(pixel(image,0,4,4) == Pixel({127,127,127,255}));
    CHECK(pixel(image,0,5,4) == Pixel({127,127,127,0}));
    // Every mip retains full encoded blocks, including 1x1 levels.
    data.assign(96,0); data[0]=11; data[32]=22; data[64]=33;
    image = gx::decode_texture({5,3,gx::TextureFormat::i8,3},bytes(data));
    CHECK(image.levels.size()==3 && image.levels[1].width==2 && image.levels[1].height==1);
    CHECK(image.levels[2].width==1 && image.levels[2].height==1);
    CHECK(pixel(image,0,0,0)==Pixel({11,11,11,11}));
    CHECK(pixel(image,1,0,0)==Pixel({22,22,22,22}));
    CHECK(pixel(image,2,0,0)==Pixel({33,33,33,33}));
    std::fill(data.begin(),data.end(),255);
    CHECK(pixel(image,0,0,0)==Pixel({11,11,11,11}));
    auto updated = gx::decode_texture({5,3,gx::TextureFormat::i8,3},bytes(data));
    CHECK(pixel(updated,0,0,0)==Pixel({255,255,255,255}));
    rejects(gx::TextureErrorCode::truncated,[&]{gx::decode_texture({5,3,gx::TextureFormat::i8,3},bytes(data).first(95));});
    rejects(gx::TextureErrorCode::truncated,[&]{gx::decode_texture({1,1,gx::TextureFormat::rgba8},bytes(data).first(63));});
    for (const gx::TextureDesc desc : {gx::TextureDesc{0,1},gx::TextureDesc{1,0},gx::TextureDesc{1025,1},gx::TextureDesc{1,std::numeric_limits<uint32_t>::max()},gx::TextureDesc{1,1,gx::TextureFormat::i8,0},gx::TextureDesc{1,1,gx::TextureFormat::i8,2}})
        rejects(gx::TextureErrorCode::descriptor,[&]{gx::decode_texture(desc,bytes(data));});
    rejects(gx::TextureErrorCode::unsupported,[&]{gx::decode_texture({1,1,static_cast<gx::TextureFormat>(8)},bytes(data));});
    // Maximum SDK extent and all eleven levels remain bounded before allocating.
    data.assign(5592576,0xA5);
    image = gx::decode_texture({1024,1024,gx::TextureFormat::rgba8,11},bytes(data));
    CHECK(image.levels.size()==11 && image.levels.back().rgba.size()==4);
    CHECK(pixel(image,0,1023,1023)==Pixel({165,165,165,165}));
    CHECK(pixel(image,10,0,0)==Pixel({165,165,165,165}));
    return 0;
}
