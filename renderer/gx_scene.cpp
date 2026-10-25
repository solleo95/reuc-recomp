#include "gx_scene.h"
#include <algorithm>
#include <unordered_set>

namespace gx {
std::shared_ptr<const SceneFrame> SceneCapture::record(FramePacket packet) {
    if(packet.scene)throw std::invalid_argument("cannot capture an expanded scene");
    const bool present=packet.present;
    current_.state=std::make_shared<SceneNode>(SceneNode{current_.state,
        std::make_shared<const FramePacket>(std::move(packet))});
    if(!present)return {};
    auto result=std::make_shared<SceneFrame>();
    result->state=std::move(current_.state);
    const auto& source=*result->state->command;
    auto& p=result->presentation;
    p.serial=source.serial; p.clear_after_copy=false; p.xfb=source.xfb;
    p.display_source=source.display_source; p.prompts=source.prompts;
    p.black_output=source.black_output; p.aim_consumed=source.aim_consumed; p.replay=source.replay;
    return result;
}
std::vector<std::shared_ptr<const FramePacket>> scene_commands(
    const SceneFrame& frame,const std::function<bool(const std::shared_ptr<const TexturePixels>&,bool)>& resident) {
    std::vector<std::shared_ptr<const FramePacket>> output;
    for(auto node=frame.state;node;node=node->previous)output.push_back(node->command);
    std::reverse(output.begin(),output.end());
    std::unordered_set<const TexturePixels*> produced;
    for(auto& command:output) {
        const auto require=[&](const std::shared_ptr<const TexturePixels>& texture) {
            if(texture && texture->gpu_only && !produced.contains(texture.get()) && !resident(texture,true))
                throw std::logic_error("previous-frame EFB texture has no render history");
        };
        for(const auto& draw:command->draws) { require(draw.texture); for(const auto& texture:draw.texture_maps)require(texture); }
        if(command->texture_copy) {
            const auto& copy=*command->texture_copy;
            if(!copy.target || !copy.target->gpu_only)throw std::invalid_argument("invalid scene copy target");
            produced.insert(copy.target.get());
            if(resident(copy.target,false)) {
                // Re-presenting an old immutable scene may reuse its existing copy.
                auto repeat=std::make_shared<FramePacket>(*command);
                repeat->display_source=copy.source; repeat->texture_copy.reset();
                repeat->clear_only=repeat->clear_after_copy;
                command=std::move(repeat);
            }
        }
    }
    return output;
}
}
