#include "check.h"
#include "native_renderer.h"
#include "vulkan_renderer.h"
#include <intrin.h>
#include <SDL3/SDL.h>
#include <chrono>
#include <cstdio>
#include <thread>
#include <algorithm>

class VulkanReference final : public uc::NativeRenderer {
    uc::VulkanRenderer renderer_;
    bool scene_targets_=false;
public:
    void initialize(SDL_Window* w,bool v,const std::filesystem::path& s,double scale,double aspect,bool exclusive) override {renderer_.initialize(w,v,s,scale,aspect,exclusive);}
    bool render(const gx::FramePacket& p,bool display,uint64_t serial) override {return renderer_.render(p,display,serial);}
    void resize(uint32_t w,uint32_t h) override {renderer_.resize(w,h);}
    void set_render_scale(double s) override {renderer_.set_render_scale(s);}
    void set_controls(const uc::NativeControls& c) override {
        renderer_.set_aim_source(c.aim);renderer_.set_aim_history(c.aim_history);renderer_.set_wake_seconds(c.wake_seconds);renderer_.set_crosshair_scale(c.crosshair_percent);
        auto e=c.effects;e.anisotropy=c.anisotropy;e.per_pixel_lighting=c.per_pixel_lighting;renderer_.set_effects(e);
        if(scene_targets_!=c.scene_targets) {CHECK(renderer_.set_upscaling(c.scene_targets ? uc::UpscalerKind::blit : uc::UpscalerKind::off,uc::UpscaleMode::native,false,.5,false,2));scene_targets_=c.scene_targets;}
    }
    void set_bloom(const uc::BloomSettings& settings) override {renderer_.set_bloom(settings);}
    void set_hdr(const uc::HdrSettings& settings) override {renderer_.set_hdr(settings);}
    void hdr_state_changed() override {renderer_.hdr_state_changed();}
    bool hdr_active() const override {return renderer_.hdr_active();}
    bool set_upscaling(uc::UpscalerKind k,uc::UpscaleMode m,bool s,double sharp,bool gen,uint32_t n) override {return renderer_.set_upscaling(k,m,s,sharp,gen,n);}
    uc::UpscalerKind active_upscaler() const override {return renderer_.active_upscaler();}
    void set_menu(std::function<void()> build,std::function<void()> setup) override {renderer_.set_menu(std::move(build),std::move(setup));}
    bool menu_event(const SDL_Event& e) override {return renderer_.menu_event(e);}
    void set_prompt_style(uc::PromptStyle style) override {renderer_.set_prompt_style(style);}
    void set_reticle_layer(bool on) override {renderer_.set_reticle_layer(on);}
    bool reticle_layer_ready() const override {return renderer_.reticle_layer_ready();}
    bool held_presented() const override {return renderer_.held_presented();}
    bool held_has_reticle() const override {return renderer_.held_has_reticle();}
    bool render_overlay(uint64_t serial) override {return renderer_.render_overlay(serial);}
    bool copy_ready(const std::shared_ptr<const gx::TexturePixels>& t,bool previous) override {return renderer_.copy_ready(t,previous);}
    std::vector<const gx::TexturePixels*> copy_keys() const override {return renderer_.copy_keys();}
    void lost_copies(std::vector<const gx::TexturePixels*> keys) override {renderer_.lost_copies(std::move(keys));}
    std::vector<uint64_t> poll_completions() override {return renderer_.poll_completions();}
    bool wait_for_pending(uint64_t ns) override {return renderer_.wait_for_pending(ns);}
    uc::RenderPixels capture(uint64_t serial,bool efb) override {return renderer_.capture(serial,efb);}
    uc::RenderPixels capture_scene() override {return renderer_.capture_scene();}
    uc::MotionPixels capture_motion() override {return renderer_.capture_motion();}
    uc::NativeStats stats() const override {uc::NativeStats s;s.presentations=renderer_.presentation_count();s.textures=renderer_.texture_creations();s.validation_errors=renderer_.validation_errors();return s;}
    void shutdown() override {renderer_.shutdown();}
};

int main(int argc,char** argv) {
    SDL_Window* window=nullptr;
    try {
        if(argc!=2 || (std::string_view(argv[1])!="dx11" && std::string_view(argv[1])!="dx12" && std::string_view(argv[1])!="vulkan")) throw std::invalid_argument("usage: native_renderer_smoke vulkan|dx11|dx12");
        const bool vulkan=std::string_view(argv[1])=="vulkan";
        CHECK(SDL_Init(SDL_INIT_VIDEO));
        window=SDL_CreateWindow("DirectX renderer parity",1280,720,SDL_WINDOW_HIDDEN|SDL_WINDOW_RESIZABLE|(vulkan ? SDL_WINDOW_VULKAN : 0));
        CHECK(window);
        std::unique_ptr<uc::NativeRenderer> renderer;
        if(vulkan) renderer=std::make_unique<VulkanReference>();
        else renderer=std::string_view(argv[1])=="dx11" ? uc::make_d3d11_renderer() : uc::make_d3d12_renderer();
        if(!vulkan) {
            bool rejected=false;
            try { renderer->initialize(window,true,std::filesystem::path(SDL_GetBasePath())/"missing-shaders",.5,4.0/3); }
            catch(const std::runtime_error& error) { rejected=std::string_view(error.what()).find("shader")!=std::string_view::npos; }
            CHECK(rejected);
            renderer->shutdown(); renderer->shutdown();
        }
        renderer->initialize(window,true,std::filesystem::path(SDL_GetBasePath())/"data/shaders",0.5,4.0/3);
        uint64_t serial=0;
        std::vector<uint64_t> completed;
        const auto submit=[&](gx::FramePacket& packet,bool display=true) {
            ++serial;
            packet.serial=serial+1000; // Detached scene replay uses an independent submission serial.
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while(!renderer->render(packet,display,serial)) {
                CHECK(std::chrono::steady_clock::now()<deadline);
                renderer->wait_for_pending(1000000);
            }
            while(std::find(completed.begin(),completed.end(),serial)==completed.end()) {
                const auto retired=renderer->poll_completions();
                completed.insert(completed.end(),retired.begin(),retired.end());
                CHECK(std::chrono::steady_clock::now()<deadline);
                if(std::find(completed.begin(),completed.end(),serial)==completed.end()) renderer->wait_for_pending(1000000);
            }
        };
        gx::FramePacket packet; packet.clear_color={17,33,65,255}; packet.clear_after_copy=false;
        submit(packet);
        auto pixels=renderer->capture(serial,true);
        CHECK(pixels.width==480 && pixels.height==480);
        CHECK(pixels.rgba.size()==size_t(pixels.width)*pixels.height*4);
        for(size_t i=0;i<pixels.rgba.size();i+=4) CHECK(pixels.rgba[i]==17 && pixels.rgba[i+1]==33 && pixels.rgba[i+2]==65 && pixels.rgba[i+3]==255);
        gx::DrawPacket draw; draw.primitive=0x90; draw.indices={0,1,2}; draw.vertices.resize(3);
        draw.vertices[0].position={-0.8f,-0.8f,0.5f}; draw.vertices[1].position={0.8f,-0.8f,0.5f}; draw.vertices[2].position={0,0.8f,0.5f};
        for(auto& vertex:draw.vertices) vertex.color[0]={231,47,19,255};
        packet.draws.push_back(draw);
        submit(packet,false);
        pixels=renderer->capture(serial,true);
        const auto at=[&](uint32_t x,uint32_t y,size_t channel) { return pixels.rgba[(size_t(y)*pixels.width+x)*4+channel]; };
        CHECK(at(240,240,0)==231 && at(240,240,1)==47 && at(240,240,2)==19);
        CHECK(at(0,0,0)==17 && renderer->stats().presentations==1);
        // Depth compare/write must prevent the farther green triangle replacing red.
        packet.draws[0].uniforms.edit().pixel[1]=1u|16u|(1u<<1);
        submit(packet,false);
        for(auto& vertex:packet.draws[0].vertices) { vertex.position[2]=0.8f; vertex.color[0]={0,255,0,255}; }
        submit(packet,false);
        pixels=renderer->capture(serial,true);
        CHECK(at(240,240,0)==231 && at(240,240,1)==47);
        uc::save_png(pixels,std::filesystem::path(SDL_GetBasePath())/(std::string("parity-")+argv[1]+".png"));
        // Immutable texture versions and all four texels exercise upload lifetime and UV orientation.
        auto texture=std::make_shared<gx::TexturePixels>();
        texture->levels.push_back({2,2,{255,0,0,255, 0,255,0,255, 0,0,255,255, 32,64,96,255}});
        packet.draws[0]=draw; packet.draws[0].texture=texture;
        for(auto& vertex:packet.draws[0].vertices) vertex.color[0]={255,255,255,255};
        for(uint32_t texel=0;texel<4;++texel) {
            for(auto& vertex:packet.draws[0].vertices) vertex.texcoord[0]={0.25f+0.5f*(texel%2),0.25f+0.5f*(texel/2)};
            submit(packet,false); pixels=renderer->capture(serial,true);
            for(size_t channel=0;channel<4;++channel) CHECK(at(240,240,channel)==texture->levels[0].rgba[texel*4+channel]);
        }
        auto changed=std::make_shared<gx::TexturePixels>(*texture); changed->levels[0].rgba[12]=224;
        packet.draws[0].texture=changed; submit(packet,false); pixels=renderer->capture(serial,true); CHECK(at(240,240,0)==224);
        packet.draws[0].texture=texture; submit(packet,false); pixels=renderer->capture(serial,true); CHECK(at(240,240,0)==32);
        auto copy=std::make_shared<gx::TexturePixels>(); copy->gpu_only=true; copy->guest_address=0x123400;
        copy->levels.push_back({640,480,{}});
        packet.draws.clear(); packet.present=false; packet.texture_copy=gx::TextureCopy{{0,0,640,480},copy};
        submit(packet,false);
        renderer->capture(serial,true);
        CHECK(renderer->copy_ready(copy));
        CHECK(std::find(completed.begin(),completed.end(),serial)!=completed.end());
        CHECK(!renderer->wait_for_pending(0));
        const auto repeated_copy=[&] {
            auto version=std::make_shared<gx::TexturePixels>();version->gpu_only=true;version->guest_address=copy->guest_address;
            version->levels={{8,8,{}},{4,4,{}},{2,2,{}},{1,1,{}}};
            packet.texture_copy=gx::TextureCopy{{0,0,640,480},version};submit(packet,false);packet.texture_copy.reset();
        };
        for(int i=0;i<128;++i)repeated_copy();const auto warmed_copies=renderer->stats().textures;
        for(int i=0;i<64;++i)repeated_copy();CHECK(renderer->stats().textures==warmed_copies);
        packet.texture_copy.reset();packet.draws.push_back(draw);packet.draws[0].texture=copy;
        for(auto& vertex:packet.draws[0].vertices) {vertex.color[0]={255,255,255,255};vertex.texcoord[0]={.5f,.5f};}
        submit(packet,false);pixels=renderer->capture(serial,true);CHECK(at(240,240,0)==32 && at(240,240,1)==64 && at(240,240,2)==96);
        auto lost_copy=std::make_shared<gx::TexturePixels>(*copy); // New renderer has no resident GPU image for this copy.
        renderer->lost_copies({lost_copy.get()});packet.draws[0].texture=lost_copy;
        submit(packet,false);pixels=renderer->capture(serial,true);
        for(size_t channel=0;channel<4;++channel)CHECK(at(240,240,channel)==0);
        packet.draws.clear();
        packet.texture_copy.reset(); packet.present=true;
        renderer->resize(0,0);
        packet.serial=serial+1; CHECK(!renderer->render(packet));
        renderer->resize(1280,720); submit(packet);
        renderer->capture(serial);
        bool rejected=false;
        try { renderer->render(packet,true,serial); } catch(const std::invalid_argument&) { rejected=true; }
        CHECK(rejected);
        // Shared scene/boundary/HUD fixture: bloom must stay off the HUD and disappear when disabled.
        const auto quad=[](float x0,float y0,float x1,float y1,uint8_t level,bool perspective) {
            gx::DrawPacket q; q.primitive=0x90; q.indices={0,1,2,0,2,3};
            for(const auto& c:std::array<std::array<float,2>,4>{{{x0,y0},{x1,y0},{x1,y1},{x0,y1}}}) {
                gx::Vertex v; v.position={c[0],c[1],perspective ? -1.f : 0.5f}; v.color[0]={level,level,level,255}; q.vertices.push_back(v);
            }
            if(perspective) q.transforms.edit().projection={{{1,0,0,0},{0,1,0,0},{0,0,-4.f/3,-2.f/3},{0,0,-1,0}}};
            q.state.edit().viewport=std::array<float,6>{0,0,640,360,0,1};
            q.uniforms.edit().pixel[1]=0;
            return q;
        };
        // GX TEV registers, destination alpha and logic operations share the same shader/state contract.
        gx::FramePacket clean;clean.clear_color={16,32,64,255};submit(clean);submit(clean);
        const auto gx_pixel=[&](const gx::DrawPacket& draw) {
            gx::FramePacket frame;frame.clear_color={16,32,64,255};frame.draws.push_back(draw);submit(frame);
            const auto picture=renderer->capture(serial);const size_t offset=(size_t(picture.height/2)*picture.width+picture.width/2)*4;
            return std::array<uint8_t,4>{picture.rgba[offset],picture.rgba[offset+1],picture.rgba[offset+2],picture.rgba[offset+3]};
        };
        auto tev=quad(-1,-1,1,1,255,false);tev.blend={1,4,5,3,true,true};
        for(auto& vertex:tev.vertices)vertex.color[0]={255,0,0,0};CHECK((gx_pixel(tev)==std::array<uint8_t,4>{16,32,64,255}));
        for(auto& vertex:tev.vertices)vertex.color[0]={255,0,0,128};const auto blended=gx_pixel(tev);CHECK(blended[0]>=134 && blended[0]<=137 && blended[1]>=15 && blended[1]<=17);
        tev=quad(-1,-1,1,1,255,false);
        auto red_texture=std::make_shared<gx::TexturePixels>();red_texture->levels={{1,1,{255,0,0,255}}};
        auto mask_texture=std::make_shared<gx::TexturePixels>();mask_texture->levels={{1,1,{0,255,0,64}}};
        tev.texture=red_texture;tev.texture_maps[0]=mask_texture;tev.material.stages=2;tev.material.swaps={0xE4,0xE4,0xE4,0xE4};
        tev.material.stage[0]={0x8FFF8,0x8FFC0,0x3C0,0,0};tev.material.stage[1]={0x8FFF0,0x8FFC0,0x3C1,0,0};
        CHECK((gx_pixel(tev)==std::array<uint8_t,4>{255,0,0,64}));
        tev.uniforms.edit().pixel[0]=0;CHECK((gx_pixel(tev)==std::array<uint8_t,4>{16,32,64,255}));
        tev.uniforms.edit().pixel={0x3F0000,0,256|123,2};CHECK((gx_pixel(tev)==std::array<uint8_t,4>{255,0,0,123}));
        tev.blend={1,4,5,3,true,true};const auto blended_alpha=gx_pixel(tev);CHECK(blended_alpha[0]>=75 && blended_alpha[0]<=77 && blended_alpha[3]==123);
        auto depth_background=quad(-1,-1,1,1,255,false);
        for(auto& vertex:depth_background.vertices)vertex.color[0]={16,32,64,255};
        depth_background.uniforms.edit().pixel[1]=1u|16u|(7u<<1);
        tev.uniforms.edit().pixel[1]=1u|16u; // NEVER must also reject destination alpha at equal existing depth.
        gx::FramePacket never_alpha;never_alpha.draws={depth_background,tev};submit(never_alpha);
        const auto never_pixels=renderer->capture(serial);
        const size_t never_offset=(size_t(never_pixels.height/2)*never_pixels.width+never_pixels.width/2)*4;
        CHECK(never_pixels.rgba[never_offset+3]==255);
        const auto logic=[](unsigned op,uint8_t s,uint8_t d)->uint8_t {
            switch(op) {case 0:return 0;case 1:return s&d;case 2:return s&~d;case 3:return s;case 4:return ~s&d;case 5:return d;case 6:return s^d;case 7:return s|d;
                case 8:return ~(s|d);case 9:return ~(s^d);case 10:return ~d;case 11:return s|~d;case 12:return ~s;case 13:return ~s|d;case 14:return ~(s&d);default:return 255;}
        };
        const std::array<uint8_t,4> source{0x5a,0xa5,0x3c,0xc3},destination{16,32,64,255};
        auto logical=quad(-1,-1,1,1,255,false);for(auto& vertex:logical.vertices)vertex.color[0]=source;
        for(unsigned op=0;op<16;++op) {logical.blend={2,1,0,uint8_t(op),true,true};const auto actual=gx_pixel(logical);for(size_t c=0;c<4;++c)CHECK(actual[c]==logic(op,source[c],destination[c]));}
        auto copied565=std::make_shared<gx::TexturePixels>();copied565->gpu_only=true;copied565->channels=gx::TextureChannels::rgb565;copied565->levels={{4,4,{}},{2,2,{}},{1,1,{}}};
        gx::FramePacket copy565;copy565.present=false;copy565.clear_after_copy=false;auto uniform=quad(-1,-1,1,1,255,false);
        uniform.state.edit().viewport=std::array<float,6>{0,0,640,480,0,1};for(auto& vertex:uniform.vertices)vertex.color[0]={17,33,65,19};copy565.draws.push_back(uniform);
        copy565.texture_copy=gx::TextureCopy{{0,0,640,480},copied565};submit(copy565,false);
        auto sampled565=quad(-1,-1,1,1,255,false);sampled565.texture=copied565;for(auto& vertex:sampled565.vertices)vertex.texcoord[0]={.5f,.5f};
        CHECK((gx_pixel(sampled565)==std::array<uint8_t,4>{16,32,66,255}));
        const auto bloom_frame=[&](uc::BloomSettings settings,bool menu=false) {
            renderer->set_bloom(settings);
            if(!menu) {
                gx::FramePacket scene; scene.present=false; scene.clear_after_copy=false;
                scene.draws.push_back(quad(-1,-1,1,1,20,true)); scene.draws.push_back(quad(-.2f,-.2f,.2f,.2f,255,true)); submit(scene,false);
                gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
                boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target}; submit(boundary,false);
            }
            gx::FramePacket hud; hud.display_source={0,0,640,360};
            auto red=quad(.6f,-.9f,.9f,-.6f,0,false); for(auto& vertex:red.vertices) vertex.color[0]={255,0,0,255}; hud.draws.push_back(std::move(red));
            submit(hud); return renderer->capture(serial);
        };
        bloom_frame({false,100}); const auto base=bloom_frame({false,100});
        for(size_t channel=0;channel<3;++channel) CHECK(base.rgba[(size_t(base.height-1)*base.width+base.width-1)*4+channel]==20);
        bloom_frame({true,100}); const auto glow=bloom_frame({true,100});
        const auto red_at=[](const uc::RenderPixels& p,uint32_t x,uint32_t y) {return int(p.rgba[(size_t(y)*p.width+x)*4]);};
        uint32_t edge=0; for(uint32_t x=0;x<base.width;++x) if(red_at(base,x,base.height/2)>200)edge=x;
        CHECK(edge>base.width/2 && edge+40<base.width);
        const int near_gain=red_at(glow,edge+8,glow.height/2)-red_at(base,edge+8,base.height/2);
        const int far_gain=red_at(glow,edge+40,glow.height/2)-red_at(base,edge+40,base.height/2);
        CHECK(near_gain>=3 && near_gain>far_gain && far_gain>=0);
        bloom_frame({true,200}); const auto stronger=bloom_frame({true,200});
        CHECK(red_at(stronger,edge+8,stronger.height/2)>red_at(glow,edge+8,glow.height/2));
        bloom_frame({true,0}); CHECK(bloom_frame({true,0}).rgba==base.rgba);
        bloom_frame({true,100}); CHECK(bloom_frame({true,100}).rgba==glow.rgba);
        uc::save_png(glow,std::filesystem::path(SDL_GetBasePath())/(std::string("bloom-")+argv[1]+".png"));
        bloom_frame({false,100},true); const auto menu_off=bloom_frame({false,100},true);
        bloom_frame({true,100},true); CHECK(bloom_frame({true,100},true).rgba==menu_off.rgba);
        renderer->set_bloom({false,100});renderer->set_reticle_layer(true);
        std::array<float,2> aim{};uc::NativeControls controls;controls.aim=[&] {return aim;};renderer->set_controls(controls);
        gx::FramePacket held;held.display_source={0,0,640,360};held.clear_after_copy=false;
        held.draws.push_back(quad(-1,-1,1,1,20,false));
        auto reticle=quad(-.1f,-.1f,.1f,.1f,0,false);reticle.layer=reticle.reticle=reticle.late_aim=true;
        for(auto& vertex:reticle.vertices)vertex.color[0]={255,0,0,255};held.draws.push_back(std::move(reticle));
        const auto presents_before=renderer->stats().presentations;submit(held);
        CHECK(renderer->reticle_layer_ready() && renderer->held_has_reticle() && !renderer->held_presented());
        CHECK(renderer->stats().presentations==presents_before);
        const auto unchanged_efb=renderer->capture(serial,true);
        const auto overlay=[&] {
            ++serial;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while(!renderer->render_overlay(serial)) {CHECK(std::chrono::steady_clock::now()<deadline);renderer->wait_for_pending(1000000);}
            bool done=false;while(!done) {
                const auto retired=renderer->poll_completions();done=std::find(retired.begin(),retired.end(),serial)!=retired.end();
                CHECK(std::chrono::steady_clock::now()<deadline);if(!done)renderer->wait_for_pending(1000000);
            }
            return renderer->capture(serial);
        };
        const auto centered=overlay();CHECK(red_at(centered,centered.width/2,centered.height/2)==255);
        CHECK(renderer->held_presented() && renderer->stats().presentations==presents_before+1);
        aim={.3f,0};const auto moved=overlay();CHECK(red_at(moved,moved.width/2,moved.height/2)==20);
        CHECK(renderer->capture(serial,true).rgba==unchanged_efb.rgba);
        renderer->set_reticle_layer(false);CHECK(!renderer->reticle_layer_ready());
        const auto lighting_frame=[&](bool per_pixel) {
            controls.per_pixel_lighting=per_pixel;renderer->set_controls(controls);
            gx::FramePacket p;p.display_source={0,0,640,360};auto lit=quad(-1,-1,1,1,255,true);
            for(auto& vertex:lit.vertices)vertex.normal={0,0,1,0,0,0,0,0,0};
            auto& uniforms=lit.uniforms.edit();uniforms.channels[0]=1|2|(1<<2)|(2<<7)|(3<<9);
            auto& light=uniforms.lights[0];light.color={255,255,255,255};light.cosine={1,0,0,0};light.distance={1,0,4,0};light.position={0,0,-.8f,0};light.direction={0,0,1,0};
            p.draws.push_back(std::move(lit));submit(p);return renderer->capture(serial);
        };
        const auto vertex_lit=lighting_frame(false),pixel_lit=lighting_frame(true);
        CHECK(red_at(pixel_lit,640,360)>red_at(vertex_lit,640,360)+40);
        CHECK(std::abs(red_at(pixel_lit,20,20)-red_at(vertex_lit,20,20))<=4);
        uc::save_png(pixel_lit,std::filesystem::path(SDL_GetBasePath())/(std::string("lighting-")+argv[1]+".png"));
        // HDR output keeps SDR captures stable and responds to display-state changes.
        renderer->set_hdr({true,1000,200,200});renderer->hdr_state_changed();
        CHECK(lighting_frame(true).rgba==pixel_lit.rgba);
        std::printf("%s: HDR output %s on this display\n",argv[1],renderer->hdr_active() ? "active" : "unavailable");
        renderer->resize(1280,720);CHECK(lighting_frame(true).rgba==pixel_lit.rgba);
        renderer->set_hdr({false,1000,200,200});renderer->hdr_state_changed();
        CHECK(lighting_frame(true).rgba==pixel_lit.rgba);CHECK(!renderer->hdr_active());
        controls.per_pixel_lighting=false;renderer->set_controls(controls);
        const auto antialiased=[&](uc::UpscalerKind kind,bool sharpen,double sharpness) {
            CHECK(renderer->set_upscaling(kind,uc::UpscaleMode::native,sharpen,sharpness,false,2));
            uc::RenderPixels image;
            for(int frame=0;frame<3;++frame) {
                gx::FramePacket scene;scene.present=false;scene.clear_after_copy=false;
                scene.draws.push_back(quad(-1,-1,1,1,80,true));
                auto diagonal=quad(-.7f,-.7f,.7f,.7f,200,true);diagonal.indices={0,1,2};
                scene.draws.push_back(std::move(diagonal));submit(scene,false);
                gx::FramePacket boundary;boundary.present=false;boundary.clear_after_copy=false;
                auto target=std::make_shared<gx::TexturePixels>();target->gpu_only=true;target->levels={{640,360,{}}};
                boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};submit(boundary,false);
                gx::FramePacket hud;hud.display_source={0,0,640,360};submit(hud);image=renderer->capture(serial);
            }
            return image;
        };
        const auto aliased=antialiased(uc::UpscalerKind::off,false,.5);
        const auto smooth=antialiased(uc::UpscalerKind::smaa,false,.5);
        CHECK(renderer->active_upscaler()==uc::UpscalerKind::smaa);
        size_t smoothed=0;
        for(size_t i=0;i<smooth.rgba.size();i+=4) if(smooth.rgba[i]>80 && smooth.rgba[i]<200 && smooth.rgba[i]!=aliased.rgba[i])++smoothed;
        CHECK(smoothed>100);
        const auto sharp=antialiased(uc::UpscalerKind::smaa,true,1);
        CHECK(sharp.rgba!=smooth.rgba);
        CHECK(std::abs(red_at(sharp,100,360)-80)<=1);
        CHECK(antialiased(uc::UpscalerKind::smaa,false,.5).rgba==smooth.rgba);
        CHECK(antialiased(uc::UpscalerKind::off,false,.5).rgba==aliased.rgba);
        uc::save_png(smooth,std::filesystem::path(SDL_GetBasePath())/(std::string("smaa-")+argv[1]+".png"));
        uc::save_png(sharp,std::filesystem::path(SDL_GetBasePath())/(std::string("rcas-")+argv[1]+".png"));
        controls.scene_targets=true;renderer->set_controls(controls);
        const auto moving_frame=[&](float shift,bool logic_overlap=false) {
            gx::FramePacket scene;scene.present=false;scene.clear_after_copy=false;
            auto object=quad(-1,-1,.1f,1,200,true);object.callsite=0x1234;
            object.transforms.edit().position_rows[0][3]=shift;
            object.uniforms.edit().pixel[1]=1u|16u|(3u<<1);
            for(auto& vertex:object.vertices)vertex.normal={0,0,1,0,0,0,0,0,0};
            if(logic_overlap) {
                object.blend={2,1,0,6,true,true};
                for(auto& vertex:object.vertices)vertex.color[0]={90,165,60,195};
                const auto front=object.vertices;
                for(auto vertex:front) {
                    for(auto& coordinate:vertex.position)coordinate*=1.5f;
                    vertex.color[0]={165,90,195,60};object.vertices.push_back(vertex);
                }
                object.indices.insert(object.indices.end(),{4,5,6,4,6,7});
            }
            scene.draws.push_back(std::move(object));submit(scene,false);
            gx::FramePacket boundary;boundary.present=false;boundary.clear_after_copy=false;
            auto target=std::make_shared<gx::TexturePixels>();target->gpu_only=true;target->levels={{640,360,{}}};
            boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};submit(boundary,false);
            gx::FramePacket hud;hud.display_source={0,0,640,360};
            hud.draws.push_back(quad(.4f,-.8f,.8f,.8f,90,false));submit(hud);
        };
        moving_frame(0);moving_frame(0);moving_frame(.025f);
        const auto motion=renderer->capture_motion();CHECK(motion.width==480 && motion.height==480);
        const auto vector=motion.at(120,180),background=motion.at(450,180);
        std::printf("%s: motion %.3f %.3f\n",argv[1],vector[0],vector[1]);
        CHECK(std::abs(vector[0]+6)<.05f && std::abs(vector[1])<.05f);
        CHECK(background[0]==0 && background[1]==0);
        const auto scene_capture=renderer->capture_scene();CHECK(scene_capture.width==480 && scene_capture.height==480);
        CHECK(red_at(scene_capture,120,180)==200 && red_at(scene_capture,400,180)!=90);
        uc::save_png(scene_capture,std::filesystem::path(SDL_GetBasePath())/(std::string("scene-")+argv[1]+".png"));
        // Overlapping primitives must update depth between logic operations while retaining motion MRTs.
        moving_frame(0,true);moving_frame(0,true);moving_frame(.025f,true);
        const auto logic_scene=renderer->capture_scene();CHECK(red_at(logic_scene,120,180)==(90^16));
        const auto logic_motion=renderer->capture_motion().at(120,180);
        CHECK(std::abs(logic_motion[0]+6)<.05f && std::abs(logic_motion[1])<.05f);
        controls.scene_targets=false;renderer->set_controls(controls);
        bool mesh_surfaces=false,directional_light=true;
        const auto surface=[&](std::array<std::array<float,3>,4> corners,std::array<float,3> normal,bool lit) {
            auto object=quad(-1,-1,1,1,180,true);object.callsite=0x1000;
            object.transforms.edit().projection={{{1,0,0,0},{0,1,0,0},{0,0,-100.f/99,-100.f/99},{0,0,-1,0}}};
            object.uniforms.edit().pixel[1]=1u|16u|(3u<<1);
            for(size_t i=0;i<4;++i) {object.vertices[i].position=corners[i];object.vertices[i].normal={normal[0],normal[1],normal[2],0,0,0,0,0,0};}
            if(lit) {
                auto& u=object.uniforms.edit();u.channels[0]=1|2|(1<<2)|(2<<7);u.ambient[0]={90,90,90,255};
                auto& light=u.lights[0];light.color={255,255,255,255};light.direction={0,0,1,0};light.position={-1e6f,1e6f,1e6f,0};light.distance={1,0,0,0};light.cosine={1,0,0,0};
                if(!directional_light)light.position={-.5f,1,-3,0};
            }
            if(mesh_surfaces) {object.mesh=std::make_shared<const gx::MeshBuffers>(gx::MeshBuffers{object.vertices,object.indices});object.vertices.clear();object.indices.clear();}
            return object;
        };
        const auto occluded=[&](uc::EffectLevel ao,bool contact,bool box) {
            controls.effects.ambient_occlusion=ao;controls.effects.contact_shadows=contact;renderer->set_controls(controls);
            uc::RenderPixels result;
            for(int frame=0;frame<3;++frame) {
                gx::FramePacket scene;scene.present=false;scene.clear_after_copy=false;
                scene.draws.push_back(surface({{{-12,-1,-2},{12,-1,-2},{12,-1,-8},{-12,-1,-8}}},{0,1,0},box));
                if(box) {
                    scene.draws.push_back(surface({{{-.5f,-.4f,-5},{.5f,-.4f,-5},{.5f,-.4f,-4},{-.5f,-.4f,-4}}},{0,1,0},true));
                    scene.draws.push_back(surface({{{-.5f,-1,-4},{.5f,-1,-4},{.5f,-.4f,-4},{-.5f,-.4f,-4}}},{0,0,1},true));
                    scene.draws.push_back(surface({{{.5f,-1,-4},{.5f,-1,-5},{.5f,-.4f,-5},{.5f,-.4f,-4}}},{1,0,0},true));
                } else scene.draws.push_back(surface({{{-12,-1,-8},{12,-1,-8},{12,6,-8},{-12,6,-8}}},{0,0,1},false));
                submit(scene,false);
                gx::FramePacket boundary;boundary.present=false;boundary.clear_after_copy=false;
                auto target=std::make_shared<gx::TexturePixels>();target->gpu_only=true;target->levels={{640,360,{}}};
                boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};submit(boundary,false);
                gx::FramePacket hud;hud.display_source={0,0,640,360};hud.draws.push_back(quad(.6f,-.9f,.9f,-.6f,255,false));submit(hud);
                result=renderer->capture(serial);
            }
            return result;
        };
        const auto ao_base=occluded(uc::EffectLevel::off,false,false);
        for(const auto level:{uc::EffectLevel::low,uc::EffectLevel::medium,uc::EffectLevel::high,uc::EffectLevel::ultra}) {
            const auto shaded=occluded(level,false,false);int darkest=0;
            for(uint32_t y=300;y<335;++y)darkest=std::max(darkest,red_at(ao_base,640,y)-red_at(shaded,640,y));
            std::printf("%s: GTAO %s crease %d\n",argv[1],std::string(uc::effect_level_name(level)).c_str(),darkest);
            CHECK(darkest>=10);CHECK(std::abs(red_at(ao_base,640,190)-red_at(shaded,640,190))<=6);
            CHECK(std::abs(red_at(ao_base,640,600)-red_at(shaded,640,600))<=6);
            if(level==uc::EffectLevel::high)uc::save_png(shaded,std::filesystem::path(SDL_GetBasePath())/(std::string("gtao-")+argv[1]+".png"));
        }
        const auto contact_base=occluded(uc::EffectLevel::off,false,true),contact=occluded(uc::EffectLevel::off,true,true);
        int shadow=0;for(uint32_t y=262;y<300;++y)for(uint32_t x=700;x<760;++x)shadow=std::max(shadow,red_at(contact_base,x,y)-red_at(contact,x,y));
        std::printf("%s: contact shadow %d\n",argv[1],shadow);CHECK(shadow>=15);
        for(uint32_t x=100;x<500;x+=20)CHECK(std::abs(red_at(contact_base,x,200)-red_at(contact,x,200))<=1);
        uc::save_png(contact,std::filesystem::path(SDL_GetBasePath())/(std::string("contact-")+argv[1]+".png"));
        controls.effects={};
        const auto blurred=[&](uc::EffectLevel level) {
            controls.effects.motion_blur=level;renderer->set_controls(controls);
            moving_frame(0);moving_frame(0);moving_frame(.125f);return renderer->capture(serial);
        };
        const auto blur_off=blurred(uc::EffectLevel::off),blur_on=blurred(uc::EffectLevel::high);
        size_t blur_pixels=0;for(uint32_t x=600;x<790;++x)if(red_at(blur_on,x,360)!=red_at(blur_off,x,360))++blur_pixels;
        std::printf("%s: motion blur changed %zu edge pixels\n",argv[1],blur_pixels);CHECK(blur_pixels>=8);
        moving_frame(.125f);CHECK(renderer->capture(serial).rgba==blur_off.rgba);
        uc::save_png(blur_on,std::filesystem::path(SDL_GetBasePath())/(std::string("blur-")+argv[1]+".png"));
        controls.effects={};renderer->set_controls(controls);
        mesh_surfaces=true;
        const auto shadow_base=occluded(uc::EffectLevel::off,false,true);
        for(const auto level:{uc::EffectLevel::low,uc::EffectLevel::medium,uc::EffectLevel::high,uc::EffectLevel::ultra}) {
            controls.effects.shadows=level;const auto shaded=occluded(uc::EffectLevel::off,false,true);int darkness=0;
            for(uint32_t y=262;y<340;++y)for(uint32_t x=640;x<900;++x)darkness=std::max(darkness,red_at(shadow_base,x,y)-red_at(shaded,x,y));
            std::printf("%s: shadow map %s darkness %d\n",argv[1],std::string(uc::effect_level_name(level)).c_str(),darkness);CHECK(darkness>=25);
            for(uint32_t x=100;x<500;x+=20)CHECK(std::abs(red_at(shadow_base,x,200)-red_at(shaded,x,200))<=1);
            if(level==uc::EffectLevel::high)uc::save_png(shaded,std::filesystem::path(SDL_GetBasePath())/(std::string("shadow-")+argv[1]+".png"));
        }
        directional_light=false;controls.effects.shadows=uc::EffectLevel::off;const auto point_base=occluded(uc::EffectLevel::off,false,true);
        controls.effects.shadows=uc::EffectLevel::medium;CHECK(occluded(uc::EffectLevel::off,false,true).rgba==point_base.rgba);
        controls.effects={};renderer->set_controls(controls);
        auto cached_draw=quad(-.4f,-.4f,.4f,.4f,200,false);
        cached_draw.mesh=std::make_shared<const gx::MeshBuffers>(gx::MeshBuffers{cached_draw.vertices,cached_draw.indices});
        cached_draw.vertices.clear(); cached_draw.indices.clear(); cached_draw.state.edit().descriptors[0]=gx::Mode::direct;
        gx::FramePacket cached_packet; cached_packet.draws.push_back(cached_draw); submit(cached_packet);
        cached_packet.draws[0].transforms.edit().loaded_rows.reset(1);
        rejected=false;
        try { renderer->render(cached_packet,true,serial+1); } catch(const gx::Error& error) { rejected=error.code==gx::ErrorCode::layout; }
        CHECK(rejected); // An immutable mesh cache never makes changed per-draw transforms trusted.
        const auto statistics=renderer->stats();
        CHECK(statistics.validation_errors==0);
        renderer->shutdown(); renderer.reset();
        SDL_DestroyWindow(window); SDL_Quit();
        std::printf("%s: GX, textures, depth, EFB copy, retirement, resize, serial, bloom and held-frame parity passed\n",argv[1]);
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"native renderer smoke: %s\n",error.what());
        if(window) SDL_DestroyWindow(window);
        SDL_Quit(); return 1;
    }
}
