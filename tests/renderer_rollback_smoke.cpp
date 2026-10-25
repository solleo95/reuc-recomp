#include "check.h"
#include "vulkan_renderer.h"
#include "gx_scene.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
using namespace std::chrono_literals;

static std::filesystem::path executable_dir() {
    std::vector<wchar_t> path(32768);
    const DWORD size=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
    if(!size || size>=path.size()) throw std::runtime_error("executable path unavailable");
    return std::filesystem::path(std::wstring(path.data(),size)).parent_path();
}

static gx::DrawPacket quad(std::array<uint8_t,4> color) {
    gx::DrawPacket draw;
    draw.primitive=0x90;
    draw.vertices.resize(4); draw.indices={0,1,2,0,2,3};
    draw.vertices[0].position={-1,-1,0}; draw.vertices[1].position={1,-1,0};
    draw.vertices[2].position={1,1,0}; draw.vertices[3].position={-1,1,0};
    for(auto& vertex:draw.vertices) { vertex.color[0]=color; vertex.texcoord[0]={.5f,.5f}; }
    return draw;
}

static void center_is(const uc::RenderPixels& pixels,std::array<uint8_t,4> color) {
    CHECK(pixels.width && pixels.height && pixels.rgba.size()==size_t(pixels.width)*pixels.height*4);
    const size_t center=(size_t(pixels.height/2)*pixels.width+pixels.width/2)*4;
    CHECK(std::equal(color.begin(),color.end(),pixels.rgba.begin()+center));
}

int main(int argc,char** argv) {
    bool validation=false;
    for(int i=1;i<argc;++i) {
        if(std::string(argv[i])=="--validation") validation=true;
        else { std::fprintf(stderr,"usage: renderer_rollback_smoke [--validation]\n"); return 2; }
    }
    SDL_Window* window=nullptr;
    try {
        if(!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
        window=SDL_CreateWindow("Umbrella Chronicles - rollback GPU smoke",320,240,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
        if(!window) throw std::runtime_error(SDL_GetError());
        const auto hwnd=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
        if(!hwnd) throw std::runtime_error("smoke window has no native handle");
        ShowWindow(hwnd,SW_SHOWNOACTIVATE);
        uc::VulkanRenderer renderer;
        renderer.initialize(window,validation,executable_dir()/"data"/"shaders");
        uint64_t next_completion=1;
        const auto collect=[&] {
            for(auto serial:renderer.poll_completions()) { CHECK(serial==next_completion); ++next_completion; }
        };
        const auto submit=[&](gx::FramePacket& work,bool display=true,bool wait=true) {
            const auto deadline=std::chrono::steady_clock::now()+3s;
            bool submitted=false;
            while(!submitted || (wait && next_completion<=work.serial)) {
                CHECK(std::chrono::steady_clock::now()<deadline);
                SDL_Event event;
                while(SDL_PollEvent(&event)) CHECK(event.type!=SDL_EVENT_QUIT);
                if(!submitted) submitted=renderer.render(work,display);
                if(wait || !submitted) collect();
                std::this_thread::sleep_for(1ms);
            }
            if(wait) CHECK(renderer.poll_completions().empty());
        };
        const std::array<uint8_t,4> red{255,0,0,255},green{0,255,0,255},blue{0,0,255,255},black{0,0,0,255};
        gx::FramePacket work;
        work.serial=1; work.clear_after_copy=false; work.draws.push_back(quad(red));
        submit(work); center_is(renderer.capture(1),red);
        CHECK(renderer.presentation_count()==1);

        work.serial=2; work.draws[0]=quad(green);
        submit(work,false); center_is(renderer.capture(2,true),green);
        CHECK(renderer.presentation_count()==1);
        work.serial=3; work.draws.clear();
        submit(work); center_is(renderer.capture(3),green);
        CHECK(renderer.presentation_count()==2);

        work.serial=4; work.clear_after_copy=true; work.clear_color=blue;
        submit(work,false); center_is(renderer.capture(4,true),blue);
        CHECK(renderer.presentation_count()==2);
        work.serial=5; work.clear_after_copy=false;
        submit(work); center_is(renderer.capture(5),blue);
        CHECK(renderer.presentation_count()==3);

        auto copy=std::make_shared<gx::TexturePixels>();
        copy->gpu_only=true; copy->levels={{4,4,{}}};
        work.serial=6; work.clear_after_copy=true; work.clear_color=black;
        work.draws.push_back(quad(red)); work.texture_copy=gx::TextureCopy{{0,0,640,480},copy};
        submit(work,false); center_is(renderer.capture(6,true),black);
        CHECK(renderer.presentation_count()==3);
        work.serial=7; work.clear_after_copy=false; work.texture_copy.reset();
        work.draws[0]=quad({255,255,255,255}); work.draws[0].texture=copy;
        submit(work); center_is(renderer.capture(7),red);
        CHECK(renderer.presentation_count()==4);

        // Two hidden submissions share one completion drain; the reused displayed slot loses its capture.
        work.serial=8; work.draws.clear(); submit(work,false,false);
        work.serial=9; work.clear_after_copy=true; work.clear_color=blue; submit(work,false);
        CHECK(renderer.presentation_count()==4);
        bool unavailable=false;
        try { renderer.capture(9); } catch(const std::runtime_error&) { unavailable=true; }
        CHECK(unavailable);
        work.serial=10; work.clear_after_copy=false;
        submit(work); center_is(renderer.capture(10),blue);
        CHECK(renderer.presentation_count()==5 && next_completion==11);
        CHECK(renderer.poll_completions().empty());
        // Skip physical rendering of an entire frame, then reconstruct its copy on demand.
        gx::SceneCapture scene;
        auto deferred=std::make_shared<gx::TexturePixels>();
        deferred->gpu_only=true; deferred->guest_address=0x2000; deferred->levels={{4,4,{}}};
        gx::FramePacket source;
        source.present=false; source.draws.push_back(quad(red));
        source.texture_copy=gx::TextureCopy{{0,0,640,480},deferred}; source.clear_color=blue;
        CHECK(!scene.record(std::move(source)));
        const auto saved_scene=scene.checkpoint();
        gx::FramePacket display_copy;
        display_copy.draws.push_back(quad({255,255,255,255})); display_copy.draws.back().texture=deferred;
        const auto image=scene.record(std::move(display_copy));
        const auto render_scene=[&](const gx::SceneFrame& frame) {
            for(const auto& command:gx::scene_commands(frame,[&](const auto& texture,bool previous){return renderer.copy_ready(texture,previous);})) {
                auto packet=*command; packet.serial=next_completion; submit(packet,packet.present);
            }
        };
        render_scene(*image); center_is(renderer.capture(next_completion-1),red);
        scene.restore(saved_scene);
        gx::FramePacket corrected; corrected.draws.push_back(quad(green));
        render_scene(*scene.record(std::move(corrected)));
        center_is(renderer.capture(next_completion-1),green);
        // The first immutable scene remains valid after restore and later presentation.
        render_scene(*image); center_is(renderer.capture(next_completion-1),red);
        const auto render_sequence=[&](std::vector<gx::FramePacket> sequence,bool expanded) {
            if(expanded) {
                gx::SceneCapture capture;
                std::shared_ptr<const gx::SceneFrame> frame;
                for(auto& packet:sequence)if(auto presented=capture.record(std::move(packet)))frame=std::move(presented);
                CHECK(frame); render_scene(*frame);
            } else {
                for(auto& packet:sequence) { packet.serial=next_completion; submit(packet,packet.present); }
            }
            return renderer.capture(next_completion-1);
        };
        std::vector<gx::FramePacket> partial(3);
        partial[0].present=false; partial[0].clear_only=true; partial[0].clear_color=red;
        partial[1].present=false; partial[1].clear_only=true; partial[1].clear_color=green;
        partial[1].display_source={0,0,640,240};
        partial[2].clear_after_copy=false;
        const auto partial_direct=render_sequence(partial,false);
        const auto partial_expanded=render_sequence(partial,true);
        CHECK(partial_direct.rgba==partial_expanded.rgba);
        const auto top=(size_t(partial_expanded.height/4)*partial_expanded.width+partial_expanded.width/2)*4;
        const auto bottom=(size_t(partial_expanded.height*3/4)*partial_expanded.width+partial_expanded.width/2)*4;
        CHECK(std::equal(green.begin(),green.end(),partial_expanded.rgba.begin()+top));
        CHECK(std::equal(red.begin(),red.end(),partial_expanded.rgba.begin()+bottom));

        const auto alpha_sequence=[&] {
            auto version=std::make_shared<gx::TexturePixels>();
            version->gpu_only=true; version->guest_address=0x3000; version->levels={{4,4,{}}};
            std::vector<gx::FramePacket> sequence(5);
            for(auto& packet:sequence) { packet.present=false; packet.clear_after_copy=false; }
            sequence[0].clear_only=true; sequence[0].clear_after_copy=true; sequence[0].clear_color=blue;
            auto before=quad({255,0,0,128}); before.blend={1,4,5,3,true,false};
            sequence[1].draws.push_back(std::move(before));
            sequence[2].texture_copy=gx::TextureCopy{{0,0,640,480},version};
            sequence[2].clear_after_copy=true; sequence[2].clear_color=black;
            auto sample=quad({255,255,255,255}); sample.texture=version;
            sequence[3].draws.push_back(std::move(sample));
            auto after=quad({0,255,0,128}); after.blend={1,4,5,3,true,false};
            sequence[4].draws.push_back(std::move(after)); sequence[4].present=true;
            return sequence;
        };
        const auto alpha_direct=render_sequence(alpha_sequence(),false);
        const auto alpha_expanded=render_sequence(alpha_sequence(),true);
        CHECK(alpha_direct.rgba==alpha_expanded.rgba);
        const auto alpha_center=(size_t(alpha_expanded.height/2)*alpha_expanded.width+alpha_expanded.width/2)*4;
        CHECK(alpha_expanded.rgba[alpha_center]>=62 && alpha_expanded.rgba[alpha_center]<=65);
        CHECK(alpha_expanded.rgba[alpha_center+1]>=127 && alpha_expanded.rgba[alpha_center+1]<=129);
        CHECK(alpha_expanded.rgba[alpha_center+2]>=62 && alpha_expanded.rgba[alpha_center+2]<=65);
        CHECK(alpha_expanded.rgba[alpha_center+3]==255);
        gx::SceneCapture feedback;
        const auto record_feedback=[&](std::array<uint8_t,4> color) {
            auto version=std::make_shared<gx::TexturePixels>();
            version->gpu_only=true; version->guest_address=0x4000; version->levels={{4,4,{}}};
            gx::FramePacket source;
            source.present=false; source.clear_color=black; source.draws.push_back(quad(color));
            source.texture_copy=gx::TextureCopy{{0,0,640,480},version};
            CHECK(!feedback.record(std::move(source)));
            gx::FramePacket display;
            display.clear_color=black; display.draws.push_back(quad({255,255,255,255}));
            display.draws.back().texture=version;
            return std::pair{std::move(version),feedback.record(std::move(display))};
        };
        const auto render_feedback=[&](const gx::SceneFrame& frame,size_t expected_copies) {
            const auto commands=gx::scene_commands(frame,[&](const auto& texture,bool previous) { return renderer.copy_ready(texture,previous); });
            CHECK(size_t(std::count_if(commands.begin(),commands.end(),[](const auto& command) { return command->texture_copy.has_value(); }))==expected_copies);
            for(const auto& command:commands) { auto packet=*command; packet.serial=next_completion; submit(packet,packet.present); }
            return renderer.capture(next_completion-1);
        };
        auto materialized=record_feedback(red);
        center_is(render_feedback(*materialized.second,1),red);
        materialized.first.reset(); materialized.second.reset();
        auto skipped=record_feedback(green);
        for(int i=0;i<2;++i)skipped=record_feedback(green);
        gx::FramePacket previous_feedback;
        previous_feedback.clear_color=black; previous_feedback.draws.push_back(quad({255,255,255,255}));
        previous_feedback.draws.back().texture=skipped.first;
        const auto previous_image=feedback.record(previous_feedback);
        // None of the skipped green recipes executes: the previous-frame version aliases rendered red.
        center_is(render_feedback(*previous_image,0),red);
        const auto alias_checkpoint=feedback.checkpoint();
        auto current_feedback=record_feedback(blue);
        center_is(render_feedback(*current_feedback.second,1),blue);
        // Blue replaced the latest address entry, but the old alias must retain its immutable red target.
        feedback.restore(alias_checkpoint);
        const auto restored_feedback=feedback.record(std::move(previous_feedback));
        center_is(render_feedback(*restored_feedback,0),red);
        // Leaving a scene must release copies at addresses the next scene never uses.
        std::weak_ptr<const gx::TexturePixels> abandoned_copy;
        {
            auto target=std::make_shared<gx::TexturePixels>();
            target->gpu_only=true; target->guest_address=0x5000; target->levels={{4,4,{}}};
            abandoned_copy=target;
            gx::SceneCapture transient;
            gx::FramePacket packet;
            packet.draws.push_back(quad(red));
            packet.texture_copy=gx::TextureCopy{{0,0,640,480},target};
            render_scene(*transient.record(std::move(packet)));
        }
        for(int i=0;i<32;++i) {
            gx::FramePacket empty; empty.serial=next_completion; submit(empty);
        }
        CHECK(abandoned_copy.expired());
        renderer.shutdown(); CHECK(renderer.validation_errors()==0);
        SDL_DestroyWindow(window); window=nullptr; SDL_Quit();
        std::puts("Rollback GPU smoke passed: hidden EFB/clear/copy, completion order/once, normal presentation");
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Rollback GPU smoke failed: %s\n",error.what());
        if(window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
}
