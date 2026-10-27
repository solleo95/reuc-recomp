#pragma once
#include "native_renderer.h"
#include "renderer_backend.h"
#include "vulkan_renderer.h"

namespace uc {
// Keeps the game's renderer lifetime stable across backend/device replacement.
class GameRenderer {
public:
    explicit GameRenderer(RendererBackend backend=RendererBackend::vulkan) : backend_(backend) {
        switch(backend) {
        case RendererBackend::vulkan: vulkan_=std::make_unique<VulkanRenderer>(); break;
        case RendererBackend::directx11: native_=make_d3d11_renderer(); break;
        case RendererBackend::directx12: native_=make_d3d12_renderer(); break;
        default: throw std::invalid_argument("unknown renderer backend");
        }
    }
    void initialize(SDL_Window* window,bool validation,const std::filesystem::path& shaders,double scale,double aspect,bool exclusive) {
        if(vulkan_) vulkan_->initialize(window,validation,shaders,scale,aspect,exclusive);
        else { native_->initialize(window,validation,shaders,scale,aspect,exclusive); native_->set_controls(controls_); }
        initialized_=true; native_gpu_window_ns_=0;
    }
    bool render(const gx::FramePacket& packet,bool display=true,uint64_t serial=0) { return vulkan_ ? vulkan_->render(packet,display,serial) : native_->render(packet,display,serial); }
    bool copy_ready(const std::shared_ptr<const gx::TexturePixels>& texture,bool previous=false) {return vulkan_ ? vulkan_->copy_ready(texture,previous) : native_->copy_ready(texture,previous);}
    std::vector<const gx::TexturePixels*> copy_keys() const {return vulkan_ ? vulkan_->copy_keys() : native_->copy_keys();}
    void lost_copies(std::vector<const gx::TexturePixels*> keys) {if(vulkan_)vulkan_->lost_copies(std::move(keys));else native_->lost_copies(std::move(keys));}
    void resize(uint32_t width,uint32_t height) {if(vulkan_)vulkan_->resize(width,height);else native_->resize(width,height);}
    void set_render_scale(double scale) {if(vulkan_)vulkan_->set_render_scale(scale);else native_->set_render_scale(scale);}
    void set_aim_source(std::function<std::array<float,2>()> source) {if(vulkan_)vulkan_->set_aim_source(std::move(source));else {controls_.aim=std::move(source);apply_controls();}}
    void set_aim_history(std::function<std::array<float,2>(double)> history) {if(vulkan_)vulkan_->set_aim_history(std::move(history));else {controls_.aim_history=std::move(history);apply_controls();}}
    void set_wake_seconds(double seconds) {if(vulkan_)vulkan_->set_wake_seconds(seconds);else {controls_.wake_seconds=seconds;apply_controls();}}
    void set_crosshair_scale(uint32_t percent) {if(vulkan_)vulkan_->set_crosshair_scale(percent);else {if(percent>100)throw std::invalid_argument("invalid crosshair scale");controls_.crosshair_percent=percent;apply_controls();}}
    void set_prompt_style(PromptStyle style) {if(vulkan_)vulkan_->set_prompt_style(style);else native_->set_prompt_style(style);}
    void warm_pipelines(const std::function<void(size_t,size_t)>& progress) {if(vulkan_)vulkan_->warm_pipelines(progress);else if(progress)progress(1,1);}
    void set_menu(std::function<void()> build,std::function<void()> setup={}) {if(vulkan_)vulkan_->set_menu(std::move(build),std::move(setup));else native_->set_menu(std::move(build),std::move(setup));}
    bool menu_event(const SDL_Event& event) {return vulkan_ ? vulkan_->menu_event(event) : native_->menu_event(event);}
    void set_bloom(const BloomSettings& settings) {if(vulkan_)vulkan_->set_bloom(settings);else native_->set_bloom(settings);}
    void set_hdr(const HdrSettings& settings) {if(vulkan_)vulkan_->set_hdr(settings);else native_->set_hdr(settings);}
    void hdr_state_changed() {if(vulkan_)vulkan_->hdr_state_changed();else native_->hdr_state_changed();}
    void allow_dlss5(bool allowed) {if(vulkan_)vulkan_->allow_dlss5(allowed);else native_->allow_dlss5(allowed && backend_!=RendererBackend::directx11);}   // the DLSS 5 runtime rejects D3D11
    void set_dlss5(const Dlss5Settings& settings) {
        if(vulkan_)vulkan_->set_dlss5(settings);
        else {auto limited=settings;if(backend_==RendererBackend::directx11)limited.hook=Dlss5Hook::off;native_->set_dlss5(limited);}
    }
    bool dlss5_active() const {return vulkan_ ? vulkan_->dlss5_active() : native_->dlss5_active();}
    std::string dlss5_status() const {return vulkan_ ? vulkan_->dlss5_status() : native_->dlss5_status();}
    void set_effects(const GraphicsEffects& effects) {
        if(vulkan_) {vulkan_->set_effects(effects);return;}
        if(!valid_anisotropy(effects.anisotropy))throw std::invalid_argument("invalid anisotropy");
        controls_.effects=effects;controls_.anisotropy=effects.anisotropy;controls_.per_pixel_lighting=effects.per_pixel_lighting;
        if(backend_==RendererBackend::directx11) {   // D3D11 has no DXR: a saved or live ray tracing choice is dropped here, never reaches the backend (it throws on it)
            auto& e=controls_.effects;
            e.ray_tracing=e.ray_reconstruction=e.path_tracing=false;
            e.rt_reflections=e.rt_lighting=e.rt_shadows=e.rt_local_shadows=e.rt_ambient_occlusion=EffectLevel::off;
        }
        apply_controls();
    }
    void configure_upscaling(UpscalerKind kind,UpscaleMode mode,bool sharpening,double sharpness,bool generation,uint32_t multiplier) {
        if(vulkan_)vulkan_->configure_upscaling(kind,mode,sharpening,sharpness,generation,multiplier);
        else { limit_dx11(kind,generation); native_->set_upscaling(kind,mode,sharpening,sharpness,generation,multiplier); }
    }
    bool set_upscaling(UpscalerKind kind,UpscaleMode mode,bool sharpening,double sharpness,bool generation,uint32_t multiplier) {
        if(vulkan_)return vulkan_->set_upscaling(kind,mode,sharpening,sharpness,generation,multiplier);
        limit_dx11(kind,generation);
        return native_->set_upscaling(kind,mode,sharpening,sharpness,generation,multiplier);
    }
    UpscalerKind active_upscaler() const {return vulkan_ ? vulkan_->active_upscaler() : native_->active_upscaler();}
    bool frame_generation_active() const {return vulkan_ ? vulkan_->frame_generation_active() : native_->frame_generation_active();}
    void set_reticle_layer(bool on) {if(vulkan_)vulkan_->set_reticle_layer(on);else native_->set_reticle_layer(on);}
    bool reticle_layer_ready() const {return vulkan_ ? vulkan_->reticle_layer_ready() : native_->reticle_layer_ready();}
    bool held_presented() const {return vulkan_ ? vulkan_->held_presented() : native_->held_presented();}
    bool held_has_reticle() const {return vulkan_ ? vulkan_->held_has_reticle() : native_->held_has_reticle();}
    bool render_overlay(uint64_t serial) {return vulkan_ ? vulkan_->render_overlay(serial) : native_->render_overlay(serial);}
    std::vector<uint64_t> poll_completions() {if(!initialized_)return {};return vulkan_ ? vulkan_->poll_completions() : native_->poll_completions();}
    bool wait_for_pending(uint64_t timeout_ns) {return initialized_ && (vulkan_ ? vulkan_->wait_for_pending(timeout_ns) : native_->wait_for_pending(timeout_ns));}
    RenderPixels capture(uint64_t serial,bool efb=false) {return vulkan_ ? vulkan_->capture(serial,efb) : native_->capture(serial,efb);}
    RenderPixels capture_scene() {return vulkan_ ? vulkan_->capture_scene() : native_->capture_scene();}
    MotionPixels capture_motion() {return vulkan_ ? vulkan_->capture_motion() : native_->capture_motion();}
    uint64_t presentation_count() const {return vulkan_ ? vulkan_->presentation_count() : stats().presentations;}
    uint64_t texture_creations() const {return vulkan_ ? vulkan_->texture_creations() : stats().textures;}
    uint64_t descriptor_pool_creations() const {return vulkan_ ? vulkan_->descriptor_pool_creations() : stats().descriptor_pools;}
    uint64_t late_aim_draws() const {return vulkan_ ? vulkan_->late_aim_draws() : stats().late_aim_draws;}
    uint64_t gpu_busy_ns() const {return vulkan_ ? vulkan_->gpu_busy_ns() : stats().gpu_busy_ns;}
    uint32_t validation_errors() const {return vulkan_ ? vulkan_->validation_errors() : stats().validation_errors;}
    VulkanRenderer::FrameTimings last_timings() const {if(vulkan_)return vulkan_->last_timings();const auto s=stats();return {0,s.submit_ms,s.present_ms};}
    double take_gpu_window_ms() {
        if(vulkan_)return vulkan_->take_gpu_window_ms();
        const auto total=stats().gpu_busy_ns;
        const auto elapsed=total>=native_gpu_window_ns_ ? total-native_gpu_window_ns_ : total;
        native_gpu_window_ns_=total; return double(elapsed)/1e6;
    }
    double take_pass_total_ms(GpuPass pass) {return vulkan_ ? vulkan_->take_pass_total_ms(pass) : 0;}
    double pass_ms(GpuPass pass) const {return vulkan_ ? vulkan_->pass_ms(pass) : 0;}
    uint64_t pass_runs(GpuPass pass) const {return vulkan_ ? vulkan_->pass_runs(pass) : 0;}
    RtScene::Stats rt_stats() const {return vulkan_ ? vulkan_->rt_stats() : native_->rt_stats();}
    void shutdown() {if(vulkan_)vulkan_->shutdown();else if(initialized_)native_->shutdown();initialized_=false;}
private:
    void limit_dx11(UpscalerKind& kind,bool& generation) const {   // the DX11 SDKs have no FSR and no frame generation: fall back instead of throwing
        if(backend_!=RendererBackend::directx11)return;
        if(kind==UpscalerKind::fsr)kind=UpscalerKind::off;
        generation=false;
    }
    void apply_controls() {if(initialized_)native_->set_controls(controls_);}
    NativeStats stats() const {return initialized_ ? native_->stats() : NativeStats{};}
    RendererBackend backend_;
    std::unique_ptr<VulkanRenderer> vulkan_;
    std::unique_ptr<NativeRenderer> native_;
    NativeControls controls_;
    uint64_t native_gpu_window_ns_=0;
    bool initialized_=false;
};
}
