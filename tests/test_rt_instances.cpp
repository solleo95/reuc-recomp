#include "check.h"
#include "rt_scene.h"
int main() {
    constexpr uint32_t always=(7u<<16)|(7u<<19);   // both alpha compares "always": no cut-out
    constexpr auto list=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // An opaque perspective depth-writing indexed mesh triangle list is an opaque instance.
    CHECK(uc::rt_instance_opaque(true,true,true,true,always,list)==std::optional<bool>(true));
    // The same with an alpha compare other than "always" (either of the two) is an alpha cut-out: an instance, not opaque.
    CHECK(uc::rt_instance_opaque(true,true,true,true,10u|(4u<<16)|(7u<<19),list)==std::optional<bool>(false));
    CHECK(uc::rt_instance_opaque(true,true,true,true,(7u<<16)|(2u<<19),list)==std::optional<bool>(false));
    // Not an instance: non-mesh, no indices, no depth write, not perspective, a strip.
    CHECK(!uc::rt_instance_opaque(false,true,true,true,always,list));
    CHECK(!uc::rt_instance_opaque(true,false,true,true,always,list));
    CHECK(!uc::rt_instance_opaque(true,true,true,false,always,list));
    CHECK(!uc::rt_instance_opaque(true,true,false,true,always,list));
    CHECK(!uc::rt_instance_opaque(true,true,true,true,always,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP));
    return 0;
}
