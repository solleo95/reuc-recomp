#include "check.h"
#include "copy_scale.h"
#include <bit>
int main() {
    gx::DrawPacket draw;
    const std::array<uint32_t,3> none{};   // a variable: the commas of a braced type would split the CHECK macro's argument
    CHECK(gx::copy_scale_words(draw,4.f,4.f)==none);   // no texture: nothing to scale
    auto plain=std::make_shared<gx::TexturePixels>();
    auto copy=std::make_shared<gx::TexturePixels>(); copy->gpu_only=true;
    draw.texture=plain; draw.texture_maps[0]=plain;
    CHECK(gx::copy_scale_words(draw,4.f,4.f)==none);   // ordinary textures keep their size
    draw.texture=copy;
    auto words=gx::copy_scale_words(draw,4.f,3.f);
    CHECK(std::bit_cast<float>(words[0])==4.f && std::bit_cast<float>(words[1])==3.f && words[2]==1u);   // map 0 is an EFB copy
    draw.texture_maps[2]=copy;
    CHECK(gx::copy_scale_words(draw,4.f,3.f)[2]==(1u|(2u<<2)));   // texture_maps[2] is map 3
    draw.texture.reset();
    CHECK(gx::copy_scale_words(draw,4.f,3.f)[2]==(2u<<2));
    return 0;
}
