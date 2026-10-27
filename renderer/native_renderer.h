#pragma once
#include "gx_packets.h"
#include "render_pixels.h"
#include "bloom_math.h"
#include "hdr_math.h"
#include "upscale_mode.h"
#include "graphics_effects.h"
#include "dlss5_settings.h"
#include "rt_scene.h"
#include <functional>

struct SDL_Window;
union SDL_Event;
namespace uc {
struct NativeControls {
    std::function<std::array<float,2>()> aim;
    std::function<std::array<float,2>(double)> aim_history;
    double wake_seconds=0;
    uint32_t crosshair_percent=100, anisotropy=16;
    bool per_pixel_lighting=false;
    bool scene_targets=false;
    GraphicsEffects effects;
};
struct NativeStats {
    uint64_t presentations=0, textures=0, state_objects=0, descriptor_pools=0, late_aim_draws=0, gpu_busy_ns=0;
    uint64_t generated_presentations=0;
    uint32_t validation_errors=0;
    double submit_ms=0, present_ms=0;
};
// Native GPU boundary: immutable GX packets in, actual GPU retirement/readback out.
class NativeRenderer {
public:
    virtual ~NativeRenderer()=default;
    virtual void initialize(SDL_Window*,bool validation,const std::filesystem::path& shaders,double scale=0.5,double aspect=16.0/9,bool exclusive=false)=0;
    virtual bool render(const gx::FramePacket&,bool display=true,uint64_t submission_serial=0)=0;
    virtual void resize(uint32_t width,uint32_t height)=0;
    virtual void set_render_scale(double)=0;
    virtual void set_controls(const NativeControls&)=0;
    virtual void set_bloom(const BloomSettings&)=0;
    virtual void set_hdr(const HdrSettings&)=0;
    virtual void hdr_state_changed()=0;
    virtual bool hdr_active() const=0;
    virtual bool set_upscaling(UpscalerKind,UpscaleMode,bool,double,bool,uint32_t)=0;
    virtual UpscalerKind active_upscaler() const=0;
    virtual bool frame_generation_active() const {return false;}
    virtual void allow_dlss5(bool) {}
    virtual void set_dlss5(const Dlss5Settings& settings) {if(settings.enabled())throw std::runtime_error(dlss5_status());}
    virtual bool dlss5_active() const {return false;}
    virtual std::string dlss5_status() const {return "the supplied DLSS 5 runtime does not support DirectX 11";}
    virtual void set_menu(std::function<void()> build,std::function<void()> setup={})=0;
    virtual bool menu_event(const SDL_Event&)=0;
    virtual void set_prompt_style(PromptStyle)=0;
    virtual void set_reticle_layer(bool)=0;
    virtual bool reticle_layer_ready() const=0;
    virtual bool held_presented() const=0;
    virtual bool held_has_reticle() const=0;
    virtual bool render_overlay(uint64_t serial)=0;
    virtual bool copy_ready(const std::shared_ptr<const gx::TexturePixels>&,bool previous_frame=false)=0;
    virtual std::vector<const gx::TexturePixels*> copy_keys() const=0;
    virtual void lost_copies(std::vector<const gx::TexturePixels*>)=0;
    virtual std::vector<uint64_t> poll_completions()=0;
    virtual bool wait_for_pending(uint64_t timeout_ns)=0;
    virtual RenderPixels capture(uint64_t serial,bool efb=false)=0;
    virtual RenderPixels capture_scene()=0;
    virtual MotionPixels capture_motion()=0;
    virtual NativeStats stats() const=0;
    virtual RtScene::Stats rt_stats() const {return {};}
    virtual void shutdown()=0;
};
std::unique_ptr<NativeRenderer> make_d3d11_renderer();
std::unique_ptr<NativeRenderer> make_d3d12_renderer();
}
