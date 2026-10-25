#include "check.h"
#include "gx_scene.h"
#include <set>
#include <future>
#include <chrono>

int main() {
    gx::Packets mailbox(true);
    auto simulation=std::async(std::launch::async,[&] {
        for(int i=0;i<1000;++i)mailbox.seal_batch({});
    });
    CHECK(simulation.wait_for(std::chrono::seconds(2))==std::future_status::ready);
    simulation.get();
    CHECK(mailbox.depth().queued==1);
    auto newest=mailbox.try_take(); CHECK(newest && newest->serial==1000 && newest->scene);
    CHECK(!mailbox.try_take() && mailbox.depth().outstanding==0);
    gx::SceneCapture capture;
    auto target=std::make_shared<gx::TexturePixels>();
    target->gpu_only=true; target->guest_address=0x1000;
    target->levels.push_back({8,8,{}});
    gx::FramePacket first;
    first.present=false; first.clear_after_copy=true;
    first.texture_copy=gx::TextureCopy{{0,0,640,480},target};
    first.draws.emplace_back(); first.draws.back().callsite=1;
    CHECK(!capture.record(std::move(first)));
    const auto saved=capture.checkpoint();
    gx::FramePacket second;
    second.serial=2; second.draws.emplace_back();
    second.draws.back().callsite=2; second.draws.back().texture=target;
    const auto frame=capture.record(std::move(second));
    CHECK(frame && frame->presentation.serial==2);
    auto commands=gx::scene_commands(*frame,[](const auto&,bool){return false;});
    std::set<const gx::TexturePixels*> produced;
    bool saw_first=false,saw_second=false;
    for(const auto& command:commands) {
        for(const auto& draw:command->draws) {
            if(draw.texture && draw.texture->gpu_only)CHECK(produced.contains(draw.texture.get()));
            saw_first|=draw.callsite==1; saw_second|=draw.callsite==2;
        }
        if(command->texture_copy)produced.insert(command->texture_copy->target.get());
    }
    CHECK(saw_first && saw_second && commands.back()->present);
    const auto cached=gx::scene_commands(*frame,[&](const auto& p,bool){return p==target;});
    for(const auto& command:cached)CHECK(!command->texture_copy);
    capture.restore(saved);
    gx::FramePacket replay; replay.serial=3; replay.draws.emplace_back();
    replay.draws.back().callsite=3; replay.draws.back().texture=target;
    const auto corrected=capture.record(std::move(replay));
    for(const auto& command:gx::scene_commands(*corrected,[&](const auto& p,bool){return p==target;}))
        for(const auto& draw:command->draws)CHECK(draw.callsite!=2);
    // Partial viewports never accumulate commands from preceding logical frames.
    gx::SceneCapture partial;
    for(int i=0;i<1000;++i) {
        gx::FramePacket f; f.display_source={0,0,640,360};
        f.draws.emplace_back(); f.draws.back().state.edit().scissor=std::array<int32_t,4>{0,0,640,480};
        f.draws.back().state.edit().viewport=std::array<float,6>{0,0,640,360,0,1};
        const auto image=partial.record(std::move(f));
        CHECK(gx::scene_commands(*image,[](const auto&,bool){return false;}).size()<8);
    }
    gx::SceneCapture prefix;
    gx::FramePacket a; a.present=false; a.clear_after_copy=false;
    a.draws.emplace_back(); a.draws.back().callsite=7;
    a.texture_copy=gx::TextureCopy{{0,0,640,480},target};
    prefix.record(std::move(a));
    gx::FramePacket b; b.draws.emplace_back(); b.draws.back().texture=target;
    const auto prefix_frame=prefix.record(std::move(b));
    int preceding_draws=0;
    for(const auto& command:gx::scene_commands(*prefix_frame,[](const auto&,bool){return false;}))
        for(const auto& draw:command->draws)preceding_draws+=draw.callsite==7;
    CHECK(preceding_draws==1);
    // Immutable texture ownership preserves feedback without retaining old command chains.
    gx::SceneCapture feedback;
    std::shared_ptr<gx::TexturePixels> previous;
    std::map<const gx::TexturePixels*,std::shared_ptr<const gx::TexturePixels>> resident;
    std::vector<std::weak_ptr<const gx::SceneNode>> old_frames;
    for(int i=0;i<1000;++i) {
        auto version=std::make_shared<gx::TexturePixels>();
        version->gpu_only=true; version->guest_address=0x4000;
        gx::FramePacket f; f.draws.emplace_back(); f.draws.back().texture=previous;
        f.texture_copy=gx::TextureCopy{{0,0,640,480},version};
        const auto image=feedback.record(std::move(f));
        for(const auto& command:gx::scene_commands(*image,[&](const auto& p,bool){return resident.contains(p.get());}))
            if(command->texture_copy)resident[command->texture_copy->target.get()]=command->texture_copy->target;
        std::erase_if(resident,[](const auto& entry){return entry.second.use_count()==1;});
        old_frames.push_back(image->state); previous=std::move(version);
        if(i>3)CHECK(old_frames[i-3].expired());
    }
}
