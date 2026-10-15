#include "check.h"
#include "gx_textures.h"
#include "../runtime/gx_write_watch.h"
#include <algorithm>
#include <array>
#include <limits>
#include <chrono>
#include <cstdio>

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
    rejects(gx::TextureErrorCode::unsupported,[&]{gx::decode_texture({1,1,static_cast<gx::TextureFormat>(7)},bytes(data));});
    // Palette indices follow texture tiling; palette words are big-endian.
    std::vector<uint8_t> palette(512,0);
    palette[2]=0x12; palette[3]=0xC4; palette[30]=0xAB; palette[31]=0x56;
    data.assign(64,0); data[0]=0x1F; data[32]=0xF0;
    image=gx::decode_texture({9,1,gx::TextureFormat::c4},bytes(data),{gx::PaletteFormat::ia8,bytes(palette)});
    CHECK(pixel(image,0,0,0)==Pixel({196,196,196,18}));
    CHECK(pixel(image,0,1,0)==Pixel({86,86,86,171}));
    CHECK(pixel(image,0,8,0)==Pixel({86,86,86,171}));
    palette[510]=0xF8; palette[511]=0;
    data.assign(32,255);
    image=gx::decode_texture({1,1,gx::TextureFormat::c8},bytes(data),{gx::PaletteFormat::rgb565,bytes(palette)});
    CHECK(pixel(image,0,0,0)==Pixel({255,0,0,255}));
    palette.assign(32768,0); palette[32766]=0x3A; palette[32767]=0xBC;
    image=gx::decode_texture({1,1,gx::TextureFormat::c14x2},bytes(data),{gx::PaletteFormat::rgb5a3,bytes(palette)});
    CHECK(pixel(image,0,0,0)==Pixel({170,187,204,109})); // high two index bits ignored
    rejects(gx::TextureErrorCode::truncated,[&]{gx::decode_texture({1,1,gx::TextureFormat::c8},bytes(data));});
    rejects(gx::TextureErrorCode::truncated,[&]{gx::decode_texture({1,1,gx::TextureFormat::c14x2},bytes(data),{gx::PaletteFormat::rgb5a3,bytes(palette).first(32767)});});
    rejects(gx::TextureErrorCode::unsupported,[&]{gx::decode_texture({1,1,gx::TextureFormat::c8},bytes(data),{static_cast<gx::PaletteFormat>(3),bytes(palette)});});
    // Maximum SDK extent and all eleven levels remain bounded before allocating.
    data.assign(5592576,0xA5);
    image = gx::decode_texture({1024,1024,gx::TextureFormat::rgba8,11},bytes(data));
    CHECK(image.levels.size()==11 && image.levels.back().rgba.size()==4);
    CHECK(pixel(image,0,1023,1023)==Pixel({165,165,165,165}));
    CHECK(pixel(image,10,0,0)==Pixel({165,165,165,165}));
    gx::TextureCache cache(128);
    data.assign(32,17);
    auto first=cache.snapshot(0x80001000,{1,1,gx::TextureFormat::i8},bytes(data));
    CHECK(cache.snapshot(0x00001000,{1,1,gx::TextureFormat::i8},bytes(data))==first);
    CHECK(cache.decodes()==1);
    // A store the barrier does not see is picked up by the periodic full comparison; one it sees is picked up at once.
    data[0]=34;
    auto unseen=cache.snapshot(0x80001000,{1,1,gx::TextureFormat::i8},bytes(data));
    CHECK(unseen==first);
    gx::note_write_range(0x80001000,32);
    auto second=cache.snapshot(0x80001000,{1,1,gx::TextureFormat::i8},bytes(data));
    CHECK(second!=first && first->levels[0].rgba[0]==17 && second->levels[0].rgba[0]==34);
    data[0]=51;
    std::shared_ptr<const gx::TexturePixels> refreshed;
    for (int use=0;use<300 && !refreshed;++use) { auto now=cache.snapshot(0x80001000,{1,1,gx::TextureFormat::i8},bytes(data)); if (now!=second) refreshed=now; }
    CHECK(refreshed && refreshed->levels[0].rgba[0]==51);   // within 256 uses without any barrier hit
    data[0]=34; gx::note_write_range(0x80001000,32);
    data.assign(32,0); palette.assign(2,255);
    auto indexed=cache.snapshot(0x80002000,{1,1,gx::TextureFormat::c8},bytes(data),{gx::PaletteFormat::ia8,bytes(palette)});
    palette[1]=68;
    auto changed_palette=cache.snapshot(0x80002000,{1,1,gx::TextureFormat::c8},bytes(data),{gx::PaletteFormat::ia8,bytes(palette)});
    CHECK(indexed!=changed_palette && indexed->levels[0].rgba[0]==255 && changed_palette->levels[0].rgba[0]==68);
    auto other_format=cache.snapshot(0x80002000,{1,1,gx::TextureFormat::c8},bytes(data),{gx::PaletteFormat::rgb565,bytes(palette)});
    CHECK(other_format!=changed_palette);
    for (uint32_t i=0;i<30;++i) cache.snapshot(0x80003000+i*32,{1,1,gx::TextureFormat::i8},bytes(data));
    CHECK(cache.resident_bytes()<=128 && first->levels[0].rgba[0]==17);
    auto previous_decodes=cache.decodes();
    cache.snapshot(0x80001000,{1,1,gx::TextureFormat::i8},bytes(data));
    CHECK(cache.decodes()==previous_decodes+1);
    rejects(gx::TextureErrorCode::truncated,[&] { cache.snapshot(0x80001000,{1,1,gx::TextureFormat::i8},bytes(data).first(31)); });
    cache.clear(); CHECK(cache.decodes()==0 && cache.resident_bytes()==0);
    auto cleared=cache.snapshot(0x80001000,{1,1,gx::TextureFormat::i8},bytes(data));
    CHECK(cleared!=first && cache.decodes()==1);
    auto resized=cache.snapshot(0x80001000,{2,1,gx::TextureFormat::i8},bytes(data));
    CHECK(resized!=cleared && resized->levels[0].width==2);
    // Background decoding: the pixels match the synchronous decode once wait() returns, a cache hit shares the pending texture, and a decode error surfaces in wait().
    {
        gx::TextureCache async_cache; async_cache.set_async(true);
        std::vector<std::byte> source(5592576); for (size_t i=0;i<source.size();++i) source[i]=std::byte(i*7);
        const gx::TextureDesc big{1024,1024,gx::TextureFormat::rgba8,11};
        auto pending=async_cache.snapshot(0x80100000,big,source);
        CHECK(async_cache.snapshot(0x80100000,big,source)==pending && async_cache.decodes()==1);
        pending->wait(); pending->wait();
        const auto reference=gx::decode_texture(big,source);
        CHECK(pending->levels.size()==11 && pending->levels[0].rgba==reference.levels[0].rgba && pending->levels[10].rgba==reference.levels[10].rgba);
        std::vector<std::byte> indices(32,std::byte(255)), colors(2);   // index 255 is past the 1-colour palette
        auto broken=async_cache.snapshot(0x80200000,{1,1,gx::TextureFormat::c8},indices,{gx::PaletteFormat::ia8,colors});
        bool failed=false; try { broken->wait(); } catch (const gx::TextureError& error) { failed=error.code==gx::TextureErrorCode::truncated; }
        CHECK(failed);
        std::vector<std::shared_ptr<const gx::TexturePixels>> burst;
        for (uint32_t i=0;i<300;++i) burst.push_back(async_cache.snapshot(0x80300000+i*32,{1,1,gx::TextureFormat::i8},indices));
        for (auto& texture : burst) { texture->wait(); CHECK(texture->levels[0].rgba[0]==255); }
    }
    gx::TextureCache many;
    std::vector<std::shared_ptr<const gx::TexturePixels>> expected;
    for(uint32_t i=0;i<512;++i) expected.push_back(many.snapshot(0x80010000+i*32,{1,1,gx::TextureFormat::i8},bytes(data)));
    const auto began=std::chrono::steady_clock::now();
    for(uint32_t i=0;i<50000;++i) {
        const auto key=i%512;
        CHECK(many.snapshot(0x10000+key*32,{1,1,gx::TextureFormat::i8},bytes(data))==expected[key]);
    }
    CHECK(many.decodes()==512);
    std::printf("texture lookup benchmark: %lld us, 50000 hits/512 resident addresses\n",(long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-began).count());
    return 0;
}
