#include "native_renderer.h"
#include "native_tev.h"
#include <cstdlib>
#include "late_aim.h"
#include "render_scale.h"
#include "prompt_picture.h"
#include "bloom_math.h"
#include "hdr_math.h"
#include "post_effects.h"
#include "scene_boundary.h"
#include "motion_tracker.h"
#include "scene_lights.h"
#include "view_reconstruct.h"
#include "native_effects.h"
#include "shadow_math.h"
#include "native_viewport.h"
#include "upscale_mode.h"
#include "upscale_math.h"
#include "native_upscaler.h"
#include "streamline.h"
#include "AreaTex.h"
#include "SearchTex.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_sdl3.h"
#define NOMINMAX
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <intrin.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>

namespace uc {
namespace {
using Microsoft::WRL::ComPtr;
using Clock=std::chrono::steady_clock;
struct ImGuiScope {
    ImGuiContext* previous=ImGui::GetCurrentContext();
    explicit ImGuiScope(ImGuiContext* context) { ImGui::SetCurrentContext(context); }
    ~ImGuiScope() { ImGui::SetCurrentContext(previous); }
};
void check(HRESULT hr,const char* operation) {
    if(FAILED(hr)) throw std::runtime_error(std::string(operation)+" failed (HRESULT "+std::to_string(uint32_t(hr))+")");
}
std::vector<char> shader_bytes(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file || file.tellg()<=0) throw std::runtime_error("cannot read DirectX shader: "+path.string());
    std::vector<char> bytes(size_t(file.tellg())); file.seekg(0); file.read(bytes.data(),std::streamsize(bytes.size()));
    if(!file) throw std::runtime_error("truncated DirectX shader: "+path.string());
    return bytes;
}
struct Push {
    gx::Projection projection;
    uint32_t palette=0,current=0,indexed=0,alpha_pass=0,bank=UINT32_MAX,motion=UINT32_MAX;
    float jitter[2]{},viewport[4]{};
    uint32_t effects=0,record=UINT32_MAX;
    float shift[2]{};
};
static_assert(sizeof(Push)==128);
struct MotionBlock { gx::Projection previous; uint32_t palette=0,kind=0,unused[2]{}; };
static_assert(sizeof(MotionBlock)==80);
struct ShadowPush { Push push; std::array<std::byte,128> padding{}; };
static_assert(sizeof(ShadowPush)==256);
// D3D11.1 constant-buffer windows start/end on 256-byte boundaries.
struct PackedDraw {
    gx::DrawUniform uniforms;
    std::array<uint32_t,68> tev{};
    std::array<std::byte,192> state_padding{};
    Push push;
    std::array<std::byte,128> push_padding{};
};
static_assert(offsetof(PackedDraw,push)==2304 && sizeof(PackedDraw)==2560);
struct Image {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv,logic_rtv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    std::vector<ComPtr<ID3D11ShaderResourceView>> mip_srvs;
    std::vector<ComPtr<ID3D11RenderTargetView>> mip_rtvs;
    uint32_t width=0,height=0,mips=1;
};
struct Buffer { ComPtr<ID3D11Buffer> buffer; ComPtr<ID3D11ShaderResourceView> srv; size_t bytes=0; };
struct Mesh { std::weak_ptr<const gx::MeshBuffers> owner; Buffer vertices,indices; uint64_t rows=0; };
struct Texture { std::weak_ptr<const gx::TexturePixels> owner; Image image; bool initialized=false,gpu_only=false; };
struct Alias { std::weak_ptr<const gx::TexturePixels> requested; std::shared_ptr<const gx::TexturePixels> rendered; };
struct Slot {
    Buffer vertices,indices,constants,palettes,banks,motions,shadows;
    ComPtr<ID3D11Query> event,disjoint,start,stop;
    Image output,ui;
    std::vector<std::shared_ptr<const gx::TexturePixels>> textures;
    std::vector<std::shared_ptr<const gx::MeshBuffers>> meshes;
    uint64_t serial=0;
    bool pending=false,copied=false;
};
class D3D11Renderer final : public NativeRenderer {
public:
    ~D3D11Renderer() override { shutdown(); }
    void initialize(SDL_Window*,bool,const std::filesystem::path&,double,double,bool) override;
    bool render(const gx::FramePacket&,bool,uint64_t) override;
    void resize(uint32_t,uint32_t) override;
    void set_render_scale(double) override;
    void set_controls(const NativeControls&) override;
    void set_menu(std::function<void()>,std::function<void()>) override;
    bool menu_event(const SDL_Event&) override;
    void set_prompt_style(PromptStyle style) override { owner(); if(style!=PromptStyle::monochrome && style!=PromptStyle::sculpted) throw std::invalid_argument("invalid prompt style"); prompt_style_=style; }
    void set_bloom(const BloomSettings& settings) override { owner(); if(!valid_bloom_settings(settings)) throw std::invalid_argument("invalid bloom settings"); bloom_=settings; }
    void set_hdr(const HdrSettings&) override;
    void hdr_state_changed() override { owner(); resized_=true; invalidate_held(); }
    bool hdr_active() const override { owner(); return hdr_active_; }
    bool set_upscaling(UpscalerKind,UpscaleMode,bool,double,bool,uint32_t) override;
    UpscalerKind active_upscaler() const override { owner(); return upscaler_kind_; }
    void set_reticle_layer(bool) override;
    bool reticle_layer_ready() const override { owner(); return reticle_layer_ && !hdr_active_ && held_valid_ && !resized_ && width_ && height_ && held_.width==width_ && held_.height==height_; }
    bool held_presented() const override { owner(); return held_presented_; }
    bool held_has_reticle() const override { owner(); return !held_draws_.empty(); }
    bool render_overlay(uint64_t) override;
    bool copy_ready(const std::shared_ptr<const gx::TexturePixels>&,bool) override;
    std::vector<const gx::TexturePixels*> copy_keys() const override;
    void lost_copies(std::vector<const gx::TexturePixels*> keys) override { owner(); lost_.insert(keys.begin(),keys.end()); }
    std::vector<uint64_t> poll_completions() override;
    bool wait_for_pending(uint64_t) override;
    RenderPixels capture(uint64_t,bool) override;
    RenderPixels capture_scene() override;
    MotionPixels capture_motion() override;
    NativeStats stats() const override;
    void shutdown() override;
private:
    void owner() const { if(thread_!=std::this_thread::get_id()) throw std::logic_error("DirectX renderer used from a different thread"); }
    void collect();
    void drain();
    void rebuild();
    Image image(uint32_t,uint32_t,uint32_t=1,DXGI_FORMAT=DXGI_FORMAT_R8G8B8A8_UNORM,const D3D11_SUBRESOURCE_DATA* =nullptr,bool=false);
    void upload(Buffer&,const void*,size_t,UINT,bool=false,UINT=16);
    Buffer immutable(const void*,size_t,UINT);
    Texture& texture(const std::shared_ptr<const gx::TexturePixels>&);
    ID3D11SamplerState* sampler(uint8_t,bool,bool=false);
    void state(const gx::DrawPacket&,bool,bool=false,bool=false);
    void blit(const Image&,Image&,const std::array<uint32_t,4>&,uint32_t=0,bool=true);
    void clear_region(const gx::FramePacket&,const std::array<uint32_t,4>&,bool=false);
    void unbind_images();
    void sweep();
    void initialize_overlay();
    void draw_overlay(const gx::FramePacket&,Image&);
    void shutdown_overlay();
    void bloom_scene(const gx::SceneBoundary&);
    void antialias_scene(const gx::SceneBoundary&);
    void occlude_scene(const gx::SceneBoundary&);
    void blur_scene(const gx::SceneBoundary&);
    void encode_hdr(const gx::FramePacket&,const Image&,const Image*);
    void hdr_metadata();
    void ensure_scene();
    void ensure_shadow();
    void reset_scene();
    void reconstruct_scene(const gx::SceneBoundary&,bool);
    void upscale_depth();
    void sharpen_scene(const gx::SceneBoundary&);
    void ensure_held_frame();
    void invalidate_held();
    std::thread::id thread_=std::this_thread::get_id();
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11Device1> device1_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11DeviceContext1> context1_;
    ComPtr<ID3D11InfoQueue> info_;
    mutable UINT64 reported_messages_=0;
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<ID3D11VertexShader> vertex_,blit_vertex_;
    ComPtr<ID3D11PixelShader> pixel_,logic_pixel_,blit_pixel_;
    ComPtr<ID3D11VertexShader> motion_vertex_;
    ComPtr<ID3D11PixelShader> motion_pixel_,motion_logic_pixel_;
    ID3D11BlendState1* last_blend_=nullptr; ID3D11DepthStencilState* last_depth_=nullptr; ID3D11RasterizerState* last_raster_=nullptr;   // what state() bound last
    std::unique_ptr<TevShaders> tev_;   // TEV-specialized pixel shaders; the ubershaders above draw until one is compiled
    Image scene_color_,scene_depth_,scene_motion_,scene_gbuffer_;
    ComPtr<ID3D11DepthStencilView> scene_depth_view_;
    bool scene_initialized_=false;
    struct SceneClear { std::array<uint32_t,4> region; std::array<uint8_t,4> color; uint32_t depth; };
    std::optional<SceneClear> scene_clear_;
    std::unique_ptr<Streamline> streamline_;
    std::unique_ptr<NativeUpscaler> native_upscaler_;
    UpscaleMode upscale_mode_=UpscaleMode::native;
    bool dlss_available_=false,upscale_reset_=true;
    std::string dlss_reason_;
    uint32_t frame_index_=0,upscale_frame_=0;
    std::array<float,2> jitter_{};
    gx::Projection upscale_projection_{};
    ComPtr<ID3D11VertexShader> depth_vertex_;
    ComPtr<ID3D11PixelShader> depth_pixel_;
    Buffer depth_constants_;
    std::unique_ptr<gx::MotionTracker> tracker_=std::make_unique<gx::MotionTracker>();
    std::optional<gx::Projection> scene_projection_;
    bool scene_captured_=false,scene_tracking_=false,scene_cleared_=false;
    bool blur_history_valid_=false;
    gx::SceneLightCollector scene_lights_;
    std::optional<gx::CollectedLight> dominant_light_;
    std::optional<gx::CollectedLight> shadow_light_;
    std::optional<ShadowFit> shadow_fit_;
    Image shadow_map_;
    ComPtr<ID3D11DepthStencilView> shadow_view_;
    ComPtr<ID3D11DepthStencilState> shadow_depth_;
    ComPtr<ID3D11RasterizerState> shadow_raster_;
    ComPtr<ID3D11VertexShader> shadow_vertex_;
    ComPtr<ID3D11InputLayout> shadow_layout_;
    bool shadow_cleared_=false;
    std::array<ComPtr<ID3D11ComputeShader>,4> occlusion_shaders_;
    std::array<ComPtr<ID3D11ComputeShader>,2> blur_shaders_;
    Buffer occlusion_constants_,occlusion_shadow_,blur_constants_;
    Image ao_raw_,ao_filtered_,contact_,ao_color_,blur_tiles_,effect_color_;
    ComPtr<ID3D11VertexShader> hdr_vertex_;
    ComPtr<ID3D11PixelShader> hdr_pixel_;
    Buffer hdr_constants_;
    HdrSettings hdr_;
    bool hdr_active_=false,hdr_metadata_dirty_=true,bloom_valid_=false;
    std::array<ComPtr<ID3D11ComputeShader>,4> bloom_shaders_;
    std::array<ComPtr<ID3D11ComputeShader>,3> smaa_shaders_;
    ComPtr<ID3D11ComputeShader> rcas_shader_;
    Buffer smaa_constants_,rcas_constants_;
    Image smaa_source_,smaa_edges_,smaa_weights_,smaa_output_,smaa_area_,smaa_search_;
    UpscalerKind upscaler_kind_=UpscalerKind::off;
    bool sharpening_=false;
    double sharpness_=0.5;
    ComPtr<ID3D11SamplerState> bloom_sampler_;
    ComPtr<ID3D11InputLayout> layout_;
    ComPtr<ID3D11Texture2D> depth_;
    ComPtr<ID3D11DepthStencilView> depth_view_;
    Image efb_,backbuffer_,white_,black_;
    Image held_;
    ComPtr<ID3D11Texture2D> held_depth_;
    ComPtr<ID3D11DepthStencilView> held_depth_view_;
    std::vector<gx::DrawPacket> frame_reticle_,held_draws_;
    std::vector<gx::PromptDraw> held_prompts_;
    std::array<uint32_t,4> held_source_{0,0,640,480};
    std::array<float,2> held_aim_{};
    bool reticle_layer_=false,held_valid_=false,held_presented_=false;
    Buffer blit_constants_,clear_constants_,clear_vertices_,clear_indices_,clear_palette_;
    Buffer bloom_constants_;
    std::array<Image,6> bloom_down_;
    std::array<Image,5> bloom_up_;
    Image bloom_color_;
    BloomLevel bloom_region_{};
    BloomSettings bloom_{false,100};
    gx::SceneBoundary boundary_;
    std::optional<gx::TextureCopy> scene_restore_copy_;
    bool frame_start_=true,scene_phase_=false;
    std::array<Slot,32> slots_;
    std::vector<uint64_t> completed_;
    std::unordered_map<const gx::TexturePixels*,Texture> textures_;
    std::vector<Image> recycled_copies_;
    std::unordered_map<const gx::TexturePixels*,Alias> aliases_;
    std::unordered_map<uint32_t,std::weak_ptr<const gx::TexturePixels>> latest_;
    std::unordered_map<const gx::MeshBuffers*,Mesh> meshes_;
    std::set<const gx::TexturePixels*> lost_;
    std::map<uint32_t,ComPtr<ID3D11BlendState1>> blends_;
    std::map<uint32_t,ComPtr<ID3D11DepthStencilState>> depths_;
    std::map<uint32_t,ComPtr<ID3D11RasterizerState>> rasters_;
    std::map<uint32_t,ComPtr<ID3D11SamplerState>> samplers_;
    NativeControls controls_;
    SDL_Window* window_=nullptr;
    ImGuiContext* overlay_=nullptr;
    std::function<void()> menu_build_,menu_setup_;
    std::vector<char> font_bytes_;
    PromptStyle prompt_style_=PromptStyle::monochrome;
    NativeStats stats_;
    uint64_t last_serial_=0;
    size_t next_slot_=0;
    uint32_t width_=0,height_=0;
    double scale_=0.5,aspect_=16.0/9;
    bool initialized_=false,resized_=true,efb_initialized_=false,logic_ops_=false,tearing_=false;
};

Image D3D11Renderer::image(uint32_t width,uint32_t height,uint32_t mips,DXGI_FORMAT format,const D3D11_SUBRESOURCE_DATA* data,bool storage) {
    if(!width || !height || width>16384 || height>16384 || !mips || mips>std::bit_width(std::max(width,height))) throw std::invalid_argument("invalid DirectX image extent");
    Image result; result.width=width; result.height=height; result.mips=mips;
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=width; desc.Height=height; desc.MipLevels=mips; desc.ArraySize=1;
    const bool logic_views=!data && format==DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Format=logic_views ? DXGI_FORMAT_R8G8B8A8_TYPELESS : format; desc.SampleDesc.Count=1; desc.Usage=data ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|(data ? 0 : D3D11_BIND_RENDER_TARGET)|(storage ? D3D11_BIND_UNORDERED_ACCESS : 0);
    desc.MiscFlags=!data && mips>1 ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    check(device_->CreateTexture2D(&desc,data,&result.texture),"create texture");
    D3D11_SHADER_RESOURCE_VIEW_DESC sample{}; sample.Format=format; sample.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; sample.Texture2D.MipLevels=mips;
    check(device_->CreateShaderResourceView(result.texture.Get(),&sample,&result.srv),"create texture view");
    if(storage) { D3D11_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=format; view.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D; check(device_->CreateUnorderedAccessView(result.texture.Get(),&view,&result.uav),"create compute image view"); }
    if(!data) {
        D3D11_RENDER_TARGET_VIEW_DESC view{}; view.Format=format; view.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D; check(device_->CreateRenderTargetView(result.texture.Get(),&view,&result.rtv),"create render target");
        if(logic_views) { view.Format=DXGI_FORMAT_R8G8B8A8_UINT; check(device_->CreateRenderTargetView(result.texture.Get(),&view,&result.logic_rtv),"create logic target"); view.Format=format; }
        if(mips>1) for(uint32_t mip=0;mip<mips;++mip) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=format; srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MostDetailedMip=mip; srv.Texture2D.MipLevels=1;
            result.mip_srvs.emplace_back(); check(device_->CreateShaderResourceView(result.texture.Get(),&srv,&result.mip_srvs.back()),"copy mip sample view");
            view.Texture2D.MipSlice=mip; result.mip_rtvs.emplace_back(); check(device_->CreateRenderTargetView(result.texture.Get(),&view,&result.mip_rtvs.back()),"copy mip target view");
        }
    }
    return result;
}
void D3D11Renderer::initialize(SDL_Window* window,bool validation,const std::filesystem::path& shaders,double scale,double aspect,bool exclusive) {
    owner(); if(initialized_) throw std::logic_error("DirectX renderer already initialized");
    window_=window; aspect_=aspect; manual_render_extent(scale,aspect); scale_=device_render_scale(scale,16384,aspect);
    // Loaded for every run, as on Vulkan: DLSS then switches live, without a new device (a restart costs seconds).
    streamline_=Streamline::load(shaders.parent_path().parent_path(),false,dlss_reason_,false,RendererBackend::directx11);
    if(!streamline_ && upscaler_kind_==UpscalerKind::dlss) throw std::runtime_error("D3D11 DLSS unavailable: "+dlss_reason_);
    const auto hwnd=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
    if(!hwnd) throw std::runtime_error("SDL window has no Win32 HWND");
    const UINT flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT|(validation ? D3D11_CREATE_DEVICE_DEBUG : 0);
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0}; D3D_FEATURE_LEVEL selected;
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,flags,levels,2,D3D11_SDK_VERSION,&device_,&selected,&context_),"create D3D11 hardware device");
    check(device_.As(&device1_),"D3D11.1 device"); check(context_.As(&context1_),"D3D11.1 context");
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{}; check(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,&options,sizeof options),"D3D11 options");
    if(!options.ConstantBufferOffsetting) throw std::runtime_error("D3D11 constant buffer offsets unavailable");
    logic_ops_=options.OutputMergerLogicOp!=FALSE;
    if(validation) check(device_.As(&info_),"D3D11 validation queue");
    ComPtr<IDXGIDevice> dxgi; check(device_.As(&dxgi),"DXGI device");
    ComPtr<IDXGIAdapter> adapter; check(dxgi->GetAdapter(&adapter),"DXGI adapter");
    if(streamline_) {
        DXGI_ADAPTER_DESC adapter_desc{}; check(adapter->GetDesc(&adapter_desc),"DLSS adapter identity"); std::array<uint8_t,8> luid{}; static_assert(sizeof(adapter_desc.AdapterLuid)==8); std::memcpy(luid.data(),&adapter_desc.AdapterLuid,8);
        dlss_available_=streamline_->set_native_device(device_.Get(),dlss_reason_) && streamline_->native_dlss_supported(luid,dlss_reason_);
        if(!dlss_available_) {
            if(upscaler_kind_==UpscalerKind::dlss) throw std::runtime_error("D3D11 DLSS unavailable: "+dlss_reason_);
            streamline_.reset();   // no NVIDIA support on this adapter: the other settings do not need it
        }
    }
    ComPtr<IDXGIFactory2> factory; check(adapter->GetParent(IID_PPV_ARGS(&factory)),"DXGI factory");
    ComPtr<IDXGIFactory5> factory5; if(SUCCEEDED(factory.As(&factory5))) { BOOL available=FALSE; if(SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&available,sizeof available))) tearing_=available!=FALSE; }
    int w=0,h=0; if(!SDL_GetWindowSizeInPixels(window,&w,&h)) throw std::runtime_error(SDL_GetError()); width_=uint32_t(std::max(1,w)); height_=uint32_t(std::max(1,h));
    DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=width_; desc.Height=height_; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD; desc.Scaling=DXGI_SCALING_STRETCH; desc.Flags=tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if(streamline_) {
        // SR also requires the Present hook to retire tags and SDK resources.
        void* upgraded=factory.Detach(); std::string reason;
        const bool ok=streamline_->upgrade_native_interface(&upgraded,reason);
        factory.Attach(static_cast<IDXGIFactory2*>(upgraded));
        if(!ok) throw std::runtime_error(reason);
    }
    check(factory->CreateSwapChainForHwnd(device_.Get(),hwnd,&desc,nullptr,nullptr,&swapchain_),"create D3D11 swapchain");
    check(factory->MakeWindowAssociation(hwnd,DXGI_MWA_NO_ALT_ENTER),"disable DXGI Alt-Enter");
    if(exclusive) check(swapchain_->SetFullscreenState(TRUE,nullptr),"exclusive D3D11 presentation");
    const auto vs=shader_bytes(shaders/"smoke.dx11.vert.cso"),ps=shader_bytes(shaders/"smoke.dx11.frag.cso");
    check(device_->CreateVertexShader(vs.data(),vs.size(),nullptr,&vertex_),"GX vertex shader"); check(device_->CreatePixelShader(ps.data(),ps.size(),nullptr,&pixel_),"GX pixel shader");
    const auto mvs=shader_bytes(shaders/"smoke.motion.dx11.vert.cso"),mps=shader_bytes(shaders/"smoke.motion.dx11.frag.cso");
    check(device_->CreateVertexShader(mvs.data(),mvs.size(),nullptr,&motion_vertex_),"GX motion vertex shader"); check(device_->CreatePixelShader(mps.data(),mps.size(),nullptr,&motion_pixel_),"GX motion pixel shader");
    const auto lps=shader_bytes(shaders/"smoke.logic.dx11.frag.cso"),mlps=shader_bytes(shaders/"smoke.motion.logic.color.dx11.frag.cso");
    check(device_->CreatePixelShader(lps.data(),lps.size(),nullptr,&logic_pixel_),"GX logic pixel shader"); check(device_->CreatePixelShader(mlps.data(),mlps.size(),nullptr,&motion_logic_pixel_),"GX motion logic pixel shader");
    tev_=std::make_unique<TevShaders>(shaders,shaders.parent_path(),[device=device_.Get()](const TevShaders::Bytecode& code) -> std::shared_ptr<void> {   // runs on the compile thread: device creation calls are free-threaded
        ComPtr<ID3D11PixelShader> shader;
        if(FAILED(device->CreatePixelShader(code->data(),code->size(),nullptr,&shader))) return nullptr;
        return std::make_shared<ComPtr<ID3D11PixelShader>>(std::move(shader));
    },std::getenv("UC_DX_SPEC_SYNC")!=nullptr);
    const auto svs=shader_bytes(shaders/"smoke.shadow.dx11.vert.cso"); check(device_->CreateVertexShader(svs.data(),svs.size(),nullptr,&shadow_vertex_),"GX shadow vertex shader");
    const auto bvs=shader_bytes(shaders/"blit.dx11.vert.cso"),bps=shader_bytes(shaders/"blit.dx11.frag.cso");
    check(device_->CreateVertexShader(bvs.data(),bvs.size(),nullptr,&blit_vertex_),"blit vertex shader"); check(device_->CreatePixelShader(bps.data(),bps.size(),nullptr,&blit_pixel_),"blit pixel shader");
    const auto dvs=shader_bytes(shaders/"depth.dx11.vert.cso"),dps=shader_bytes(shaders/"depth.dx11.frag.cso");
    check(device_->CreateVertexShader(dvs.data(),dvs.size(),nullptr,&depth_vertex_),"depth upscale vertex shader"); check(device_->CreatePixelShader(dps.data(),dps.size(),nullptr,&depth_pixel_),"depth upscale pixel shader");
    const auto hvs=shader_bytes(shaders/"hdr.dx11.vert.cso"),hps=shader_bytes(shaders/"hdr.dx11.frag.cso");
    check(device_->CreateVertexShader(hvs.data(),hvs.size(),nullptr,&hdr_vertex_),"HDR vertex shader"); check(device_->CreatePixelShader(hps.data(),hps.size(),nullptr,&hdr_pixel_),"HDR pixel shader");
    const std::array<const char*,4> bloom_names{"down_first","down","up","composite"};
    for(size_t i=0;i<bloom_names.size();++i) { const auto bytes=shader_bytes(shaders/(std::string("bloom.dx11.")+bloom_names[i]+".cso")); check(device_->CreateComputeShader(bytes.data(),bytes.size(),nullptr,&bloom_shaders_[i]),"bloom compute shader"); }
    const std::array<const char*,3> smaa_names{"edges","weights","blend"};
    for(size_t i=0;i<smaa_names.size();++i) { const auto bytes=shader_bytes(shaders/(std::string("smaa.dx11.")+smaa_names[i]+".cso")); check(device_->CreateComputeShader(bytes.data(),bytes.size(),nullptr,&smaa_shaders_[i]),"SMAA compute shader"); }
    const auto rcas=shader_bytes(shaders/"rcas.dx11.cso"); check(device_->CreateComputeShader(rcas.data(),rcas.size(),nullptr,&rcas_shader_),"RCAS compute shader");
    const std::array<const char*,4> occlusion_names{"ao","denoise","contact","composite"};
    for(size_t i=0;i<occlusion_names.size();++i) { const auto bytes=shader_bytes(shaders/(std::string("gtao.dx11.")+occlusion_names[i]+".cso")); check(device_->CreateComputeShader(bytes.data(),bytes.size(),nullptr,&occlusion_shaders_[i]),"occlusion compute shader"); }
    const std::array<const char*,2> blur_names{"tile","blur"};
    for(size_t i=0;i<blur_names.size();++i) { const auto bytes=shader_bytes(shaders/(std::string("motion_blur.dx11.")+blur_names[i]+".cso")); check(device_->CreateComputeShader(bytes.data(),bytes.size(),nullptr,&blur_shaders_[i]),"motion blur compute shader"); }
    D3D11_SAMPLER_DESC linear{}; linear.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR; linear.AddressU=linear.AddressV=linear.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; linear.MaxLOD=D3D11_FLOAT32_MAX; linear.MaxAnisotropy=1; linear.ComparisonFunc=D3D11_COMPARISON_ALWAYS;
    check(device_->CreateSamplerState(&linear,&bloom_sampler_),"bloom linear sampler");
    std::array<D3D11_INPUT_ELEMENT_DESC,14> elements{{
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,UINT(offsetof(gx::GpuMeshVertex,position)),D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,UINT(offsetof(gx::GpuMeshVertex,color)),D3D11_INPUT_PER_VERTEX_DATA,0},
        {"BLENDINDICES",0,DXGI_FORMAT_R32_UINT,0,UINT(offsetof(gx::GpuMeshVertex,row)),D3D11_INPUT_PER_VERTEX_DATA,0},
        {"BLENDINDICES",1,DXGI_FORMAT_R32G32_UINT,0,UINT(offsetof(gx::GpuMeshVertex,texture_matrix)),D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",1,DXGI_FORMAT_R8G8B8A8_UNORM,0,UINT(offsetof(gx::GpuMeshVertex,color1)),D3D11_INPUT_PER_VERTEX_DATA,0},
        {"NORMAL",1,DXGI_FORMAT_R32G32B32_FLOAT,0,UINT(offsetof(gx::GpuMeshVertex,object_normal)),D3D11_INPUT_PER_VERTEX_DATA,0}}};
    for(UINT i=0;i<8;++i) elements[6+i]={"TEXCOORD",3+i,DXGI_FORMAT_R32G32_FLOAT,0,UINT(offsetof(gx::GpuMeshVertex,texcoords))+8*i,D3D11_INPUT_PER_VERTEX_DATA,0};
    check(device_->CreateInputLayout(elements.data(),UINT(elements.size()),vs.data(),vs.size(),&layout_),"GX vertex layout");
    check(device_->CreateInputLayout(elements.data(),UINT(elements.size()),svs.data(),svs.size(),&shadow_layout_),"GX shadow input layout");
    for(auto& slot:slots_) {
        D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0}; check(device_->CreateQuery(&query,&slot.event),"GPU event");
        query.Query=D3D11_QUERY_TIMESTAMP_DISJOINT; check(device_->CreateQuery(&query,&slot.disjoint),"GPU clock query");
        query.Query=D3D11_QUERY_TIMESTAMP; check(device_->CreateQuery(&query,&slot.start),"GPU start timestamp"); check(device_->CreateQuery(&query,&slot.stop),"GPU stop timestamp");
    }
    const uint32_t white=0xffffffff,black=0; D3D11_SUBRESOURCE_DATA white_data{&white,4,0},black_data{&black,4,0};
    white_=image(1,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,&white_data); black_=image(1,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,&black_data);
    initialized_=true; rebuild();
}
void D3D11Renderer::upload(Buffer& buffer,const void* data,size_t bytes,UINT bind,bool structured,UINT structure_stride) {
    if(!bytes) return;
    if(bytes>128u*1024*1024 || (structured && (!structure_stride || bytes%structure_stride))) throw std::invalid_argument("oversized DirectX upload");
    if(buffer.bytes<bytes) {
        buffer={}; buffer.bytes=(bytes+255)&~size_t(255);
        if(structured) buffer.bytes=(buffer.bytes+structure_stride-1)/structure_stride*structure_stride;
        D3D11_BUFFER_DESC desc{}; desc.ByteWidth=UINT(buffer.bytes); desc.BindFlags=bind;
        desc.Usage=structured ? D3D11_USAGE_DEFAULT : D3D11_USAGE_DYNAMIC; desc.CPUAccessFlags=structured ? 0 : D3D11_CPU_ACCESS_WRITE;
        if(structured) { desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; desc.StructureByteStride=structure_stride; }
        check(device_->CreateBuffer(&desc,nullptr,&buffer.buffer),"create upload buffer");
        if(structured) check(device_->CreateShaderResourceView(buffer.buffer.Get(),nullptr,&buffer.srv),"create structured buffer view");
    }
    if(structured) { D3D11_BOX region{0,0,0,UINT(bytes),1,1}; context_->UpdateSubresource(buffer.buffer.Get(),0,&region,data,0,0); }
    else { D3D11_MAPPED_SUBRESOURCE mapped{}; check(context_->Map(buffer.buffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"map upload buffer"); std::memcpy(mapped.pData,data,bytes); context_->Unmap(buffer.buffer.Get(),0); }
}
Buffer D3D11Renderer::immutable(const void* data,size_t bytes,UINT bind) {
    if(!bytes || bytes>UINT_MAX) throw std::invalid_argument("invalid immutable mesh size");
    Buffer result; result.bytes=bytes; D3D11_BUFFER_DESC desc{}; desc.ByteWidth=UINT(bytes); desc.BindFlags=bind; desc.Usage=D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA initial{data,0,0}; check(device_->CreateBuffer(&desc,&initial,&result.buffer),"create mesh buffer"); return result;
}
void D3D11Renderer::unbind_images() { std::array<ID3D11ShaderResourceView*,8> empty{}; context_->PSSetShaderResources(0,UINT(empty.size()),empty.data()); context_->OMSetRenderTargets(0,nullptr,nullptr); }
void D3D11Renderer::drain() { if(!context_) return; context_->Flush(); while(std::any_of(slots_.begin(),slots_.end(),[](const auto& s){return s.pending;})) { collect(); std::this_thread::yield(); } }
void D3D11Renderer::rebuild() {
    drain(); unbind_images(); backbuffer_={};
    ComPtr<IDXGISwapChain3> color_chain;
    ComPtr<IDXGISwapChain4> metadata_chain;
    const bool color_available=SUCCEEDED(swapchain_.As(&color_chain));
    // Reset the previous color space before querying: DXGI reports an already-set space as supported.
    if(color_available) check(color_chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709),"reset D3D11 color space");
    UINT support=0;
    hdr_active_=hdr_.enabled && SDL_GetBooleanProperty(SDL_GetWindowProperties(window_),SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN,false) && color_available && SUCCEEDED(swapchain_.As(&metadata_chain)) && SUCCEEDED(color_chain->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,&support)) && (support&DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT);
    check(swapchain_->ResizeBuffers(0,width_,height_,hdr_active_ ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM,tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0),"resize D3D11 swapchain");
    if(color_available) check(color_chain->SetColorSpace1(hdr_active_ ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709),"D3D11 output color space");
    hdr_metadata_dirty_=true; hdr_metadata(); bloom_valid_=false;
    check(swapchain_->GetBuffer(0,IID_PPV_ARGS(&backbuffer_.texture)),"D3D11 backbuffer");
    check(device_->CreateRenderTargetView(backbuffer_.texture.Get(),nullptr,&backbuffer_.rtv),"D3D11 backbuffer view"); backbuffer_.width=width_; backbuffer_.height=height_;
    const auto visible=manual_render_extent(scale_,aspect_); const RenderExtent extent{visible.width,uint32_t(std::llround(960*scale_))};
    if(efb_.width!=extent.width || efb_.height!=extent.height) {
        efb_=image(extent.width,extent.height,1,DXGI_FORMAT_R8G8B8A8_UNORM,nullptr,true); depth_view_.Reset(); depth_.Reset();
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=extent.width; desc.Height=extent.height; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1; desc.Format=DXGI_FORMAT_R32_TYPELESS; desc.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
        D3D11_DEPTH_STENCIL_VIEW_DESC view{}; view.Format=DXGI_FORMAT_D32_FLOAT; view.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        check(device_->CreateTexture2D(&desc,nullptr,&depth_),"EFB depth"); check(device_->CreateDepthStencilView(depth_.Get(),&view,&depth_view_),"EFB depth view"); efb_initialized_=false;
        for(auto it=textures_.begin();it!=textures_.end();) { auto source=it->second.owner.lock(); if(source && source->gpu_only) { lost_.insert(it->first); it=textures_.erase(it); } else ++it; }
        aliases_.clear(); latest_.clear(); recycled_copies_.clear();
        boundary_.reset(); frame_start_=true; scene_phase_=false; scene_restore_copy_.reset();
        for(auto& level:bloom_down_) level={}; for(auto& level:bloom_up_) level={}; bloom_color_={}; bloom_region_={};
        smaa_source_={}; smaa_edges_={}; smaa_weights_={}; smaa_output_={};
    }
    for(auto& slot:slots_) { slot.output={}; slot.ui={}; slot.copied=false; }
    reset_scene();
    invalidate_held(); held_={}; held_depth_view_.Reset(); held_depth_.Reset(); resized_=false;
}
void D3D11Renderer::resize(uint32_t width,uint32_t height) { owner(); if(width_!=width || height_!=height) { width_=width; height_=height; resized_=true; invalidate_held(); } }
void D3D11Renderer::set_render_scale(double scale) { owner(); const auto actual=device_render_scale(scale,16384,aspect_); if(scale_!=actual) { scale_=actual; resized_=true; invalidate_held(); } }
void D3D11Renderer::set_controls(const NativeControls& controls) {
    owner(); if(!std::isfinite(controls.wake_seconds) || controls.wake_seconds<0) throw std::invalid_argument("invalid wake duration");
    const auto& effects=controls.effects;
    for(const auto level:{effects.ambient_occlusion,effects.shadows,effects.motion_blur,effects.rt_quality,effects.rt_reflections,effects.rt_lighting,effects.rt_shadows,effects.rt_local_shadows,effects.rt_ambient_occlusion}) if(unsigned(level)>unsigned(EffectLevel::ultra)) throw std::invalid_argument("invalid DirectX effect level");
    if(effects.ray_tracing || effects.ray_reconstruction || effects.path_tracing || effects.rt_reflections!=EffectLevel::off || effects.rt_lighting!=EffectLevel::off || effects.rt_shadows!=EffectLevel::off || effects.rt_local_shadows!=EffectLevel::off || effects.rt_ambient_occlusion!=EffectLevel::off) throw std::runtime_error("D3D11 ray-traced effects are unavailable");
    if((controls.scene_targets || needs_scene_targets(effects))!=(controls_.scene_targets || needs_scene_targets(controls_.effects))) reset_scene();
    if(effects!=controls_.effects) invalidate_held();
    if(effects.motion_blur!=controls_.effects.motion_blur) blur_history_valid_=false;
    if(effects.shadows!=controls_.effects.shadows) { shadow_fit_.reset(); shadow_light_.reset(); shadow_cleared_=false; }
    controls_=controls; controls_.anisotropy=std::clamp(controls_.anisotropy,1u,16u);
}
void D3D11Renderer::set_hdr(const HdrSettings& settings) {
    owner(); if(!valid_hdr_settings(settings)) throw std::invalid_argument("invalid HDR settings");
    if(hdr_.enabled!=settings.enabled) { resized_=true; invalidate_held(); }
    if(hdr_!=settings) hdr_metadata_dirty_=true;
    hdr_=settings;
}
bool D3D11Renderer::set_upscaling(UpscalerKind kind,UpscaleMode mode,bool sharpening,double sharpness,bool generation,uint32_t multiplier) {
    owner();
    if(unsigned(kind)>unsigned(UpscalerKind::smaa) || unsigned(mode)>unsigned(UpscaleMode::ultra_performance) || !std::isfinite(sharpness) || sharpness<0.01 || sharpness>1 || multiplier<2 || multiplier>6) throw std::invalid_argument("invalid DirectX upscaling settings");
    if(kind==UpscalerKind::fsr) throw std::runtime_error("D3D11 FSR unavailable: the supplied FidelityFX SDK has no DirectX 11 backend");
    if(generation) throw std::runtime_error("D3D11 frame generation is unavailable");
    const bool changed=upscaler_kind_!=kind || upscale_mode_!=mode;
    if(changed || sharpening_!=sharpening || sharpness_!=sharpness) invalidate_held();
    upscaler_kind_=kind; upscale_mode_=mode; sharpening_=sharpening; sharpness_=sharpness;
    if(changed) { resized_=true; upscale_reset_=true; frame_start_=true; scene_phase_=false; samplers_.clear(); }
    if(initialized_ && kind==UpscalerKind::dlss && !streamline_) return false; // Streamline must initialize before device creation.
    if(initialized_ && kind==UpscalerKind::dlss && !dlss_available_) throw std::runtime_error("D3D11 DLSS unavailable: "+dlss_reason_);
    return true;
}
void D3D11Renderer::hdr_metadata() {
    if(!hdr_metadata_dirty_) return;
    ComPtr<IDXGISwapChain4> chain;
    if(SUCCEEDED(swapchain_.As(&chain))) {
        if(hdr_active_) {
            // BT.2020 primaries and D65, normalized to 50000. DXGI maximum mastering luminance is whole nits.
            DXGI_HDR_METADATA_HDR10 metadata{};
            metadata.RedPrimary[0]=35400; metadata.RedPrimary[1]=14600;
            metadata.GreenPrimary[0]=8500; metadata.GreenPrimary[1]=39850;
            metadata.BluePrimary[0]=6550; metadata.BluePrimary[1]=2300;
            metadata.WhitePoint[0]=15635; metadata.WhitePoint[1]=16450;
            metadata.MaxMasteringLuminance=std::max(hdr_.peak_nits,hdr_.ui_nits);
            metadata.MaxContentLightLevel=UINT16(metadata.MaxMasteringLuminance);
            // No measured frame-average luminance; leave MaxFALL unspecified.
            check(chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10,sizeof metadata,&metadata),"D3D11 HDR metadata");
        } else check(chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE,0,nullptr),"clear D3D11 HDR metadata");
    }
    hdr_metadata_dirty_=false;
}
void D3D11Renderer::encode_hdr(const gx::FramePacket& packet,const Image& scene,const Image* ui) {
    struct Constants {
        int32_t origin[2],size[2],extent[2];
        float paper,peak,ui;
        uint32_t has_overlay;
        float bloom_origin[2],bloom_size[2];
        uint32_t has_bloom,reserved;
    };
    static_assert(sizeof(Constants)==64);
    Constants constants{};
    constants.size[0]=int32_t(scene.width); constants.size[1]=int32_t(scene.height);
    constants.extent[0]=int32_t(width_); constants.extent[1]=int32_t(height_);
    constants.paper=float(hdr_.paper_white_nits); constants.peak=float(hdr_.peak_nits); constants.ui=float(hdr_.ui_nits); constants.has_overlay=ui ? 1 : 0;
    if(bloom_valid_ && !packet.black_output) {
        const auto& region=packet.display_source;
        const uint32_t x=uint32_t(uint64_t(region[0])*efb_.width/640),y=uint32_t(uint64_t(region[1])*efb_.height/480);
        const uint32_t right=std::max(x+1,uint32_t(uint64_t(region[0]+region[2])*efb_.width/640)),bottom=std::max(y+1,uint32_t(uint64_t(region[1]+region[3])*efb_.height/480));
        constants.bloom_origin[0]=float(x)/bloom_region_.width; constants.bloom_origin[1]=float(y)/bloom_region_.height;
        constants.bloom_size[0]=float(right-x)/bloom_region_.width; constants.bloom_size[1]=float(bottom-y)/bloom_region_.height; constants.has_bloom=1;
    }
    hdr_metadata(); upload(hdr_constants_,&constants,sizeof constants,D3D11_BIND_CONSTANT_BUFFER); unbind_images();
    gx::DrawPacket default_draw; state(default_draw,false);
    D3D11_VIEWPORT viewport{0,0,float(width_),float(height_),0,1}; D3D11_RECT scissor{0,0,LONG(width_),LONG(height_)};
    context_->RSSetViewports(1,&viewport); context_->RSSetScissorRects(1,&scissor);
    context_->OMSetRenderTargets(1,backbuffer_.rtv.GetAddressOf(),nullptr); context_->IASetInputLayout(nullptr); context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(hdr_vertex_.Get(),nullptr,0); context_->PSSetShader(hdr_pixel_.Get(),nullptr,0);
    context_->PSSetConstantBuffers(0,1,hdr_constants_.buffer.GetAddressOf()); context_->PSSetSamplers(0,1,bloom_sampler_.GetAddressOf());
    ID3D11ShaderResourceView* inputs[]{scene.srv.Get(),ui ? ui->srv.Get() : black_.srv.Get(),constants.has_bloom ? bloom_up_[0].srv.Get() : black_.srv.Get()};
    context_->PSSetShaderResources(0,3,inputs); context_->Draw(3,0); unbind_images(); bloom_valid_=false;
}

// GX resources and raster state.
Texture& D3D11Renderer::texture(const std::shared_ptr<const gx::TexturePixels>& source) {
    source->wait();
    if(auto found=textures_.find(source.get()); found!=textures_.end() && !found->second.owner.expired()) return found->second;
    if(source->levels.empty() || source->levels.size()>15 || unsigned(source->channels)>unsigned(gx::TextureChannels::rgb565) || (!source->gpu_only && source->channels!=gx::TextureChannels::rgba) || (source->gpu_only && source->bc7)) throw std::invalid_argument("invalid GX texture");
    uint32_t w=source->levels[0].width,h=source->levels[0].height;
    if(!w || !h || w>16384 || h>16384) throw std::invalid_argument("invalid GX texture dimensions");
    std::vector<D3D11_SUBRESOURCE_DATA> initial;
    for(size_t i=0;i<source->levels.size();++i) {
        const auto& level=source->levels[i]; const size_t pitch=source->bc7 ? size_t((w+3)/4)*16 : size_t(w)*4;
        const size_t bytes=pitch*(source->bc7 ? (h+3)/4 : h);
        if(level.width!=w || level.height!=h || (source->gpu_only ? !level.rgba.empty() : level.rgba.size()!=bytes)) throw std::invalid_argument("invalid GX texture mip chain");
        initial.push_back({level.rgba.data(),UINT(pitch),0});
        if(w==1 && h==1 && i+1<source->levels.size()) throw std::invalid_argument("excess GX texture mip levels");
        w=std::max(1u,w/2); h=std::max(1u,h/2);
    }
    w=source->levels[0].width; h=source->levels[0].height;
    if(source->gpu_only) { w=std::max(1u,uint32_t((uint64_t(w)*efb_.width+320)/640)); h=std::max(1u,uint32_t((uint64_t(h)*efb_.height+240)/480)); }
    const uint32_t mips=std::min(uint32_t(source->levels.size()),uint32_t(std::bit_width(std::max(w,h))));
    Texture result; result.owner=source; result.initialized=!source->gpu_only; result.gpu_only=source->gpu_only;
    auto recycled=source->gpu_only ? std::find_if(recycled_copies_.begin(),recycled_copies_.end(),[&](const auto& entry){return entry.width==w && entry.height==h && entry.mips==mips;}) : recycled_copies_.end();
    if(recycled!=recycled_copies_.end()) { result.image=std::move(*recycled); recycled_copies_.erase(recycled); }
    else if(source->gpu_only) {
        // A retired copy can be reused before the periodic sweep. Pending slots and live aliases still pin its owner.
        std::erase_if(aliases_,[](const auto& entry){return entry.second.requested.expired();});
        const auto reusable=std::find_if(textures_.begin(),textures_.end(),[&](const auto& entry) {
            const auto& cached=entry.second;
            return cached.gpu_only && cached.owner.expired() && cached.image.width==w && cached.image.height==h && cached.image.mips==mips;
        });
        if(reusable!=textures_.end()) { result.image=std::move(reusable->second.image); textures_.erase(reusable); }
    }
    if(!result.image.texture) { result.image=image(w,h,mips,source->bc7 ? DXGI_FORMAT_BC7_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM,source->gpu_only ? nullptr : initial.data()); ++stats_.textures; }
    return textures_.insert_or_assign(source.get(),std::move(result)).first->second;
}
ID3D11SamplerState* D3D11Renderer::sampler(uint8_t wrap,bool mips,bool scene) {
    const uint32_t key=uint32_t(wrap)|(uint32_t(mips)<<8)|(controls_.anisotropy<<9)|(uint32_t(scene)<<14);
    auto found=samplers_.find(key); if(found!=samplers_.end()) return found->second.Get();
    const D3D11_TEXTURE_ADDRESS_MODE modes[]{D3D11_TEXTURE_ADDRESS_CLAMP,D3D11_TEXTURE_ADDRESS_WRAP,D3D11_TEXTURE_ADDRESS_MIRROR};
    if((wrap&3)>2 || ((wrap>>2)&3)>2 || ((wrap>>5)&3)>2) throw std::invalid_argument("invalid GX sampler");
    D3D11_SAMPLER_DESC desc{}; desc.AddressU=modes[wrap&3]; desc.AddressV=modes[(wrap>>2)&3]; desc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    const bool min=(wrap&128)!=0,mag=(wrap&16)!=0,mip=((wrap>>5)&3)==2;
    desc.Filter=D3D11_FILTER((min ? 16 : 0)|(mag ? 4 : 0)|(mip ? 1 : 0));
    if(min && controls_.anisotropy>1) desc.Filter=D3D11_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy=controls_.anisotropy; desc.ComparisonFunc=D3D11_COMPARISON_ALWAYS; desc.MaxLOD=mips && ((wrap>>5)&3) ? D3D11_FLOAT32_MAX : 0;
    if(scene && uses_jitter(upscaler_kind_)) desc.MipLODBias=float(-std::log2(upscale_ratio(upscale_mode_))-1.0);
    ComPtr<ID3D11SamplerState> result; check(device_->CreateSamplerState(&desc,&result),"GX sampler state"); ++stats_.state_objects; return samplers_.emplace(key,std::move(result)).first->second.Get();
}
void D3D11Renderer::state(const gx::DrawPacket& draw,bool alpha,bool scene,bool surface_only) {
    auto blend=draw.blend; if(alpha) blend={0,1,0,3,false,true}; else if(draw.uniforms->pixel[2]&256) blend.alpha_write=false;
    if(surface_only) { blend.mode=0; blend.color_write=blend.alpha_write=false; }
    if(blend.mode>3 || blend.source>7 || blend.destination>7 || blend.logic>15 || draw.state->cull>3) throw std::invalid_argument("invalid GX raster state");
    if(blend.mode!=1) { blend.source=1; blend.destination=0; } if(blend.mode!=2) blend.logic=3;
    const bool surface=scene && !alpha && (draw.uniforms->pixel[1]&17)==17;
    const uint32_t key=blend.mode|(blend.source<<2)|(blend.destination<<5)|(blend.logic<<8)|(uint32_t(blend.color_write)<<12)|(uint32_t(blend.alpha_write)<<13)|(uint32_t(scene)<<14)|(uint32_t(surface)<<15);
    auto found=blends_.find(key);
    if(found==blends_.end()) {
        const D3D11_BLEND src[]{D3D11_BLEND_ZERO,D3D11_BLEND_ONE,D3D11_BLEND_DEST_COLOR,D3D11_BLEND_INV_DEST_COLOR,D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA,D3D11_BLEND_DEST_ALPHA,D3D11_BLEND_INV_DEST_ALPHA};
        const D3D11_BLEND dst[]{D3D11_BLEND_ZERO,D3D11_BLEND_ONE,D3D11_BLEND_SRC_COLOR,D3D11_BLEND_INV_SRC_COLOR,D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA,D3D11_BLEND_DEST_ALPHA,D3D11_BLEND_INV_DEST_ALPHA};
        const D3D11_LOGIC_OP logic[]{D3D11_LOGIC_OP_CLEAR,D3D11_LOGIC_OP_AND,D3D11_LOGIC_OP_AND_REVERSE,D3D11_LOGIC_OP_COPY,D3D11_LOGIC_OP_AND_INVERTED,D3D11_LOGIC_OP_NOOP,D3D11_LOGIC_OP_XOR,D3D11_LOGIC_OP_OR,D3D11_LOGIC_OP_NOR,D3D11_LOGIC_OP_EQUIV,D3D11_LOGIC_OP_INVERT,D3D11_LOGIC_OP_OR_REVERSE,D3D11_LOGIC_OP_COPY_INVERTED,D3D11_LOGIC_OP_OR_INVERTED,D3D11_LOGIC_OP_NAND,D3D11_LOGIC_OP_SET};
        if(blend.mode==2 && !logic_ops_) throw std::runtime_error("D3D11 device lacks GX logic operations");
        const auto alpha_factor=[](D3D11_BLEND factor) { switch(factor) { case D3D11_BLEND_SRC_COLOR:return D3D11_BLEND_SRC_ALPHA; case D3D11_BLEND_INV_SRC_COLOR:return D3D11_BLEND_INV_SRC_ALPHA; case D3D11_BLEND_DEST_COLOR:return D3D11_BLEND_DEST_ALPHA; case D3D11_BLEND_INV_DEST_COLOR:return D3D11_BLEND_INV_DEST_ALPHA; default:return factor; } };
        D3D11_BLEND_DESC1 desc{}; auto& rt=desc.RenderTarget[0]; rt.BlendEnable=blend.mode==1 || blend.mode==3; rt.LogicOpEnable=blend.mode==2; rt.LogicOp=logic[blend.logic];
        rt.SrcBlend=blend.mode==3 ? D3D11_BLEND_ONE : src[blend.source]; rt.DestBlend=blend.mode==3 ? D3D11_BLEND_ONE : dst[blend.destination];
        rt.SrcBlendAlpha=alpha_factor(rt.SrcBlend); rt.DestBlendAlpha=alpha_factor(rt.DestBlend); rt.BlendOp=rt.BlendOpAlpha=blend.mode==3 ? D3D11_BLEND_OP_REV_SUBTRACT : D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask=(blend.color_write ? 7 : 0)|(blend.alpha_write ? 8 : 0);
        if(scene) {
            desc.IndependentBlendEnable=TRUE;
            for(size_t i=1;i<3;++i) { auto& extra=desc.RenderTarget[i]; extra=rt; extra.BlendEnable=extra.LogicOpEnable=FALSE; extra.RenderTargetWriteMask=surface ? (i==1 ? 3 : 15) : 0; }
        }
        ComPtr<ID3D11BlendState1> result; check(device1_->CreateBlendState1(&desc,&result),"GX blend state"); found=blends_.emplace(key,std::move(result)).first; ++stats_.state_objects;
    }
    if(last_blend_!=found->second.Get()) { last_blend_=found->second.Get(); context_->OMSetBlendState(last_blend_,nullptr,UINT_MAX); }   // states a draw would only set again are skipped
    uint32_t z=draw.uniforms->pixel[1]&31; if(alpha) { if((z&17)==17) z=5; else z&=~16u; } if(surface_only) z&=~16u;
    auto depth=depths_.find(z);
    if(depth==depths_.end()) { D3D11_DEPTH_STENCIL_DESC desc{}; desc.DepthEnable=(z&1)!=0; desc.DepthWriteMask=(z&16) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO; desc.DepthFunc=D3D11_COMPARISON_FUNC(((z>>1)&7)+1); ComPtr<ID3D11DepthStencilState> result; check(device_->CreateDepthStencilState(&desc,&result),"GX depth state"); depth=depths_.emplace(z,std::move(result)).first; ++stats_.state_objects; }
    if(last_depth_!=depth->second.Get()) { last_depth_=depth->second.Get(); context_->OMSetDepthStencilState(last_depth_,0); }
    const uint32_t cull=draw.state->cull; auto raster=rasters_.find(cull);
    if(raster==rasters_.end()) { D3D11_RASTERIZER_DESC desc{}; desc.FillMode=D3D11_FILL_SOLID; desc.CullMode=cull==1 ? D3D11_CULL_BACK : cull==2 ? D3D11_CULL_FRONT : D3D11_CULL_NONE; desc.FrontCounterClockwise=FALSE; desc.DepthClipEnable=TRUE; desc.ScissorEnable=TRUE; ComPtr<ID3D11RasterizerState> result; check(device_->CreateRasterizerState(&desc,&result),"GX rasterizer state"); raster=rasters_.emplace(cull,std::move(result)).first; ++stats_.state_objects; }
    if(last_raster_!=raster->second.Get()) { last_raster_=raster->second.Get(); context_->RSSetState(last_raster_); }
}
void D3D11Renderer::blit(const Image& source,Image& target,const std::array<uint32_t,4>& region,uint32_t channels,bool linear_filter) {
    unbind_images();
    struct Constants { float uv[4]; uint32_t channels,padding[3]{}; };
    const uint32_t x=uint32_t(uint64_t(region[0])*source.width/640),y=uint32_t(uint64_t(region[1])*source.height/480);
    const uint32_t right=std::max(x+1,uint32_t(uint64_t(region[0]+region[2])*source.width/640)),bottom=std::max(y+1,uint32_t(uint64_t(region[1]+region[3])*source.height/480));
    const Constants constants{{float(x)/source.width,float(y)/source.height,float(right-x)/source.width,float(bottom-y)/source.height},channels};
    upload(blit_constants_,&constants,sizeof constants,D3D11_BIND_CONSTANT_BUFFER);
    gx::DrawPacket default_draw; state(default_draw,false);
    D3D11_VIEWPORT viewport{0,0,float(target.width),float(target.height),0,1}; D3D11_RECT scissor{0,0,LONG(target.width),LONG(target.height)};
    context_->RSSetViewports(1,&viewport); context_->RSSetScissorRects(1,&scissor);
    context_->OMSetRenderTargets(1,target.rtv.GetAddressOf(),nullptr); context_->IASetInputLayout(nullptr); context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(blit_vertex_.Get(),nullptr,0); context_->PSSetShader(blit_pixel_.Get(),nullptr,0);
    context_->VSSetConstantBuffers(0,1,blit_constants_.buffer.GetAddressOf()); context_->PSSetConstantBuffers(0,1,blit_constants_.buffer.GetAddressOf());
    context_->PSSetShaderResources(0,1,source.srv.GetAddressOf()); auto* sample=sampler(linear_filter ? 128|16 : 0,false); context_->PSSetSamplers(0,1,&sample); context_->Draw(3,0);
    unbind_images();
    for(uint32_t mip=1;mip<target.mips;++mip) {
        Image from,to; from.srv=target.mip_srvs[mip-1]; from.width=std::max(1u,target.width>>(mip-1)); from.height=std::max(1u,target.height>>(mip-1));
        to.rtv=target.mip_rtvs[mip]; to.width=std::max(1u,target.width>>mip); to.height=std::max(1u,target.height>>mip);
        blit(from,to,{0,0,640,480},channels);
    }
}
void D3D11Renderer::clear_region(const gx::FramePacket& packet,const std::array<uint32_t,4>& region,bool scene) {
    auto& target=scene ? scene_color_ : efb_; auto* depth=scene ? scene_depth_view_.Get() : depth_view_.Get();
    const LONG x=LONG(uint64_t(region[0])*target.width/640),y=LONG(uint64_t(region[1])*target.height/480);
    const LONG right=std::max(x+1,LONG(uint64_t(region[0]+region[2])*target.width/640)),bottom=std::max(y+1,LONG(uint64_t(region[1]+region[3])*target.height/480));
    if(!x && !y && right==LONG(target.width) && bottom==LONG(target.height)) { float color[4]; for(size_t i=0;i<4;++i)color[i]=packet.clear_color[i]/255.f; context_->ClearRenderTargetView(target.rtv.Get(),color); context_->ClearDepthStencilView(depth,D3D11_CLEAR_DEPTH,packet.clear_depth/float(0xffffff),0); return; }
    // ClearView does not guarantee rectangular depth clears; a GX quad writes both attachments.
    gx::DrawPacket draw; draw.primitive=0x90; draw.uniforms.edit().pixel[1]=31;
    PackedDraw constants; constants.uniforms=*draw.uniforms; constants.push.projection=draw.transforms->projection;
    std::array<gx::GpuMeshVertex,3> vertices{}; const float clear_depth=packet.clear_depth/float(0xffffff);
    for(size_t i=0;i<3;++i) { gx::Vertex vertex; vertex.color[0]=packet.clear_color; vertex.position={i==1 ? 3.f : -1.f,i==2 ? 3.f : -1.f,clear_depth}; vertices[i]=gx::mesh_vertex(vertex); }
    const std::array<uint32_t,3> indices{0,1,2}; gx::PaletteBlock palette; gx::fill_palette(palette,*draw.transforms);
    upload(clear_constants_,&constants,sizeof constants,D3D11_BIND_CONSTANT_BUFFER); upload(clear_vertices_,vertices.data(),sizeof vertices,D3D11_BIND_VERTEX_BUFFER); upload(clear_indices_,indices.data(),sizeof indices,D3D11_BIND_INDEX_BUFFER); upload(clear_palette_,&palette,sizeof palette,D3D11_BIND_SHADER_RESOURCE,true);
    unbind_images(); context_->OMSetRenderTargets(1,target.rtv.GetAddressOf(),depth); state(draw,false);
    D3D11_VIEWPORT viewport{float(x),float(y),float(right-x),float(bottom-y),0,1}; D3D11_RECT scissor{x,y,right,bottom}; context_->RSSetViewports(1,&viewport); context_->RSSetScissorRects(1,&scissor);
    context_->IASetInputLayout(layout_.Get()); context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); UINT stride=sizeof(gx::GpuMeshVertex),offset=0; context_->IASetVertexBuffers(0,1,clear_vertices_.buffer.GetAddressOf(),&stride,&offset); context_->IASetIndexBuffer(clear_indices_.buffer.Get(),DXGI_FORMAT_R32_UINT,0);
    context_->VSSetShader(vertex_.Get(),nullptr,0); context_->PSSetShader(pixel_.Get(),nullptr,0);
    ID3D11Buffer* buffers[]{clear_constants_.buffer.Get(),clear_constants_.buffer.Get()}; UINT starts[]{0,144},counts[]{144,16}; context1_->VSSetConstantBuffers1(0,2,buffers,starts,counts); context1_->PSSetConstantBuffers1(0,2,buffers,starts,counts);
    context_->VSSetShaderResources(8,1,clear_palette_.srv.GetAddressOf()); context_->PSSetShaderResources(0,1,white_.srv.GetAddressOf()); auto* sample=sampler(0,false); context_->PSSetSamplers(0,1,&sample); context_->DrawIndexed(3,0,0); unbind_images();
}

// Packet submission and GPU retirement.
bool D3D11Renderer::render(const gx::FramePacket& packet,bool display,uint64_t submission_serial) {
    owner(); if(!initialized_) throw std::logic_error("DirectX renderer not initialized");
    const uint64_t serial=submission_serial ? submission_serial : packet.serial;
    if(!serial || serial<=last_serial_) throw std::invalid_argument("DirectX submission serial must increase");
    const bool overlay_frame=packet.overlay_only;
    if(overlay_frame && (!packet.present || packet.texture_copy || packet.clear_after_copy || packet.clear_only)) throw std::invalid_argument("invalid held-frame overlay packet");
    if(packet.clear_depth>0xffffff) throw std::invalid_argument("invalid GX clear depth");
    for(const auto& prompt:packet.prompts) {
        for(float value:prompt.efb_corners) if(!std::isfinite(value)) throw std::invalid_argument("nonfinite prompt geometry");
        for(float value:prompt.alpha) if(!std::isfinite(value) || value<0 || value>1) throw std::invalid_argument("invalid prompt alpha");
        for(const auto& tint:prompt.tint) for(float value:tint) if(!std::isfinite(value) || value<0 || value>1) throw std::invalid_argument("invalid prompt tint");
        for(float value:prompt.clip) if(!std::isfinite(value)) throw std::invalid_argument("nonfinite prompt clip");
        if(prompt.clip[2]<prompt.clip[0] || prompt.clip[3]<prompt.clip[1]) throw std::invalid_argument("invalid prompt clip");
    }
    if(!width_ || !height_) return false;
    if(overlay_frame && !reticle_layer_ready()) return false;
    if(resized_) rebuild(); collect();
    auto& slot=slots_[next_slot_]; if(slot.pending) return false;
    const bool layer_active=reticle_layer_ && !hdr_active_;
    const bool hold=layer_active && !overlay_frame && packet.present && display && !packet.black_output;
    const bool scene_phase=!overlay_frame && (frame_start_ ? boundary_.frame_had_scene() : scene_phase_);
    const bool scene_packet=(upscaler_kind_!=UpscalerKind::off || controls_.scene_targets || needs_scene_targets(controls_.effects)) && scene_phase;
    if(scene_packet) ensure_scene();
    const uint32_t target_width=overlay_frame ? width_ : scene_packet ? scene_color_.width : efb_.width,target_height=overlay_frame ? height_ : scene_packet ? scene_color_.height : efb_.height;
    if(hold) ensure_held_frame();
    const auto started=Clock::now(); slot.textures.clear(); slot.meshes.clear(); slot.copied=false;
    const auto validate_region=[](const std::array<uint32_t,4>& r) { if(!r[2] || !r[3] || r[0]>=640 || r[1]>=480 || r[2]>640-r[0] || r[3]>480-r[1]) throw std::invalid_argument("invalid GX copy rectangle"); };
    validate_region(packet.display_source);
    auto next_boundary=boundary_;
    const auto cut=overlay_frame ? std::optional<size_t>{} : next_boundary.on_packet(gx::summarize(packet));
    const bool scene_end=scene_phase && (cut.has_value() || packet.present);
    if(!overlay_frame && frame_start_) {
        scene_tracking_=false; scene_cleared_=false; scene_captured_=false; scene_projection_.reset(); scene_lights_.clear(); shadow_fit_.reset(); shadow_light_.reset(); shadow_cleared_=false;
        if(!scene_phase) { blur_history_valid_=false; upscale_reset_=true; }
        jitter_=scene_packet && uses_jitter(upscaler_kind_) ? jitter_offset(upscale_frame_,jitter_phase_count(scene_color_.width,efb_.width)) : std::array<float,2>{};
        if(streamline_ && upscaler_kind_==UpscalerKind::dlss) streamline_->begin_frame(frame_index_);
    }
    if(scene_packet && !scene_tracking_) { tracker_->begin_frame(); scene_tracking_=true; }
    auto restore_copy=scene_restore_copy_;
    if(scene_end && (!packet.draws.empty() || packet.present)) restore_copy.reset();
    Texture* copied=nullptr;
    if(packet.texture_copy) { validate_region(packet.texture_copy->source); const auto& target=packet.texture_copy->target; if(!target || !target->gpu_only) throw std::invalid_argument("invalid GX copy target"); copied=&texture(target); if(copied->initialized) throw std::invalid_argument("immutable GX copy cannot be overwritten"); slot.textures.push_back(target); }
    struct DrawGpu { const gx::DrawPacket* draw; ID3D11Buffer* vertices; ID3D11Buffer* indices; UINT first_vertex,first_index,count,constant,alpha_constant; std::array<ID3D11ShaderResourceView*,8> images; std::array<ID3D11SamplerState*,8> samplers; std::array<int32_t,4> viewport_clip{}; UINT shadow_constant=UINT_MAX; };
    std::vector<DrawGpu> draws; std::vector<gx::GpuMeshVertex> vertices; std::vector<uint32_t> indices; std::vector<PackedDraw> constants;
    std::vector<gx::PaletteBlock> palettes; std::vector<gx::TextureBank> banks;
    std::vector<MotionBlock> motions;
    std::vector<ShadowPush> shadow_pushes;
    std::unordered_map<const gx::Transforms*,uint32_t> palette_indices; std::unordered_map<const gx::TextureBank*,uint32_t> bank_indices;
    for(const auto& draw:packet.draws) {
        if((layer_active && !overlay_frame && draw.layer) || (overlay_frame && !draw.layer)) continue;
        const auto v=draw.vertex_view(); const auto ix=draw.index_view();
        if(ix.empty() || (draw.state->cull==3 && !(scene_packet && controls_.effects.shadows!=EffectLevel::off)) || gx::bloom_composite(draw) || (draw.reticle && !controls_.crosshair_percent)) continue;
        if(draw.material.stages>16 || v.empty() || v.size()>INT_MAX || ix.size()>UINT_MAX) throw std::invalid_argument("invalid GX draw size");
        const bool indexed=draw.state->descriptors[0]!=gx::Mode::none;
        DrawGpu info{&draw,nullptr,nullptr,0,0,UINT(ix.size()),UINT(constants.size()),UINT_MAX,{},{}};
        const bool moving_wake=draw.wake && controls_.aim_history && controls_.wake_seconds>0;
        if(draw.mesh && !moving_wake) {
            auto found=meshes_.find(draw.mesh.get());
            if(found==meshes_.end() || found->second.owner.expired()) {
                for(const auto index:ix) if(index>=v.size()) throw std::invalid_argument("GX mesh index out of bounds");
                std::vector<gx::GpuMeshVertex> converted; converted.reserve(v.size()); for(const auto& vertex:v)converted.push_back(gx::mesh_vertex(vertex));
                Mesh mesh; mesh.owner=draw.mesh; mesh.rows=gx::used_rows(v); mesh.vertices=immutable(converted.data(),converted.size()*sizeof(converted[0]),D3D11_BIND_VERTEX_BUFFER); mesh.indices=immutable(ix.data(),ix.size_bytes(),D3D11_BIND_INDEX_BUFFER); found=meshes_.insert_or_assign(draw.mesh.get(),std::move(mesh)).first;
            }
            gx::validate_draw_rows(*draw.transforms,indexed,found->second.rows,draw.callsite);
            info.vertices=found->second.vertices.buffer.Get(); info.indices=found->second.indices.buffer.Get(); slot.meshes.push_back(draw.mesh);
        } else {
            for(const auto index:ix) if(index>=v.size()) throw std::invalid_argument("GX mesh index out of bounds");
            gx::validate_draw_rows(*draw.transforms,indexed,indexed ? gx::used_rows(v) : 0,draw.callsite);
            info.first_vertex=UINT(vertices.size()); info.first_index=UINT(indices.size()); for(const auto& vertex:v) vertices.push_back(gx::mesh_vertex(vertex)); indices.insert(indices.end(),ix.begin(),ix.end());
            if(moving_wake) gx::retarget_wake(draw,vertices.data()+info.first_vertex,v.size(),controls_.wake_seconds,controls_.aim_history);
        }
        PackedDraw packed; packed.uniforms=*draw.uniforms;
        packed.tev[0]=draw.material.stages; for(size_t i=0;i<4;++i) packed.tev[1]|=uint32_t(draw.material.swaps[i])<<(8*i);
        for(size_t i=0;i<draw.material.stages;++i) { const auto& stage=draw.material.stage[i]; packed.tev[2+i*4]=stage.color; packed.tev[3+i*4]=stage.alpha; packed.tev[4+i*4]=stage.order; packed.tev[5+i*4]=stage.konst; }
        auto [palette_it,new_palette]=palette_indices.emplace(&*draw.transforms,UINT(palettes.size())); if(new_palette) { palettes.emplace_back(); gx::fill_palette(palettes.back(),*draw.transforms); }
        packed.push.projection=draw.transforms->projection; packed.push.palette=palette_it->second; packed.push.current=draw.transforms->current; packed.push.indexed=indexed;
        packed.push.effects=controls_.per_pixel_lighting ? 1u : 0u;
        if(scene_packet) {
            if(controls_.effects.contact_shadows || controls_.effects.shadows!=EffectLevel::off) scene_lights_.add(*draw.uniforms);
            const auto summary=gx::summarize(draw); const auto key=tracker_->key(draw);
            if((draw.uniforms->pixel[1]&17)==17) {
                MotionBlock motion;
                if(const auto* previous=tracker_->previous(key)) {
                    motion.kind=1; motion.previous=previous->transforms->projection;
                    const auto [it,added]=palette_indices.emplace(previous->transforms.get(),UINT(palettes.size()));
                    if(added) { palettes.emplace_back(); gx::fill_palette(palettes.back(),*previous->transforms); }
                    motion.palette=it->second;
                } else { motion.kind=2; motion.previous=tracker_->clip_to_prev_clip(); }
                packed.push.motion=UINT(motions.size()); motions.push_back(motion);
            }
            tracker_->record(key,draw.transforms.block(),summary.z_write);
            if(summary.perspective) { packed.push.jitter[0]=jitter_[0]; packed.push.jitter[1]=jitter_[1]; upscale_projection_=draw.transforms->projection; }
            if(summary.perspective && summary.z_write && !scene_projection_) {
                scene_projection_=draw.transforms->projection;
                if(controls_.effects.shadows!=EffectLevel::off && dominant_light_ && dominant_light_->kind==gx::LightKind::directional) {
                    shadow_fit_=fit_shadow(*scene_projection_,*dominant_light_,30.f,shadow_preset(controls_.effects.shadows).resolution); shadow_light_=dominant_light_;
                }
            }
        }
        if(draw.texture_bank) { auto [it,added]=bank_indices.emplace(draw.texture_bank.get(),UINT(banks.size())); if(added) banks.push_back(*draw.texture_bank); packed.push.bank=it->second; }
        constants.push_back(packed);
        const auto depth_bits=draw.uniforms->pixel[1]&31;
        if((draw.uniforms->pixel[2]&256) && draw.blend.alpha_write && !(depth_bits&1 && ((depth_bits>>1)&7)==0)) { info.alpha_constant=UINT(constants.size()); packed.push.alpha_pass=1; constants.push_back(packed); }
        for(size_t i=0;i<8;++i) {
            auto chosen=i ? draw.texture_maps[i-1] : draw.texture; const Image* image=&white_;
            if(chosen) {
                if(const auto alias=aliases_.find(chosen.get());alias!=aliases_.end() && !alias->second.requested.expired()) chosen=alias->second.rendered;
                if(!chosen) image=&black_;
                else if(lost_.contains(chosen.get())) image=&black_;
                else { auto& entry=texture(chosen); if(!entry.initialized) throw std::invalid_argument("GX texture sampled before its EFB copy"); image=&entry.image; slot.textures.push_back(chosen); }
            }
            info.images[i]=image->srv.Get(); info.samplers[i]=sampler(draw.texture_wrap[i],image->mips>1,scene_packet);
        }
        draws.push_back(info);
    }
    if(packet.texture_copy) copied=&textures_.at(packet.texture_copy->target.get());
    if(banks.empty()) banks.emplace_back();
    upload(slot.vertices,vertices.data(),vertices.size()*sizeof(gx::GpuMeshVertex),D3D11_BIND_VERTEX_BUFFER); upload(slot.indices,indices.data(),indices.size()*sizeof(uint32_t),D3D11_BIND_INDEX_BUFFER);
    upload(slot.palettes,palettes.data(),palettes.size()*sizeof(gx::PaletteBlock),D3D11_BIND_SHADER_RESOURCE,true); upload(slot.banks,banks.data(),banks.size()*sizeof(gx::TextureBank),D3D11_BIND_SHADER_RESOURCE,true);
    // Late aim is sampled after resource preparation, directly before GPU submission.
    for(auto& info:draws) {
        const auto& draw=*info.draw; auto& push=constants[info.constant].push;
        const auto v=draw.state->viewport.value_or(std::array<float,6>{0,0,640,480,0,1});
        for(float value:v) if(!std::isfinite(value)) throw std::invalid_argument("nonfinite GX viewport");
        auto area=efb_viewport(v,target_width,target_height);
        if(overlay_frame) { const float sx=target_width/float(packet.display_source[2]),sy=target_height/float(packet.display_source[3]); area={(v[0]-1.f/12-packet.display_source[0])*sx,(v[1]-1.f/12-packet.display_source[1])*sy,v[2]*sx,v[3]*sy}; }
        const float original_width=area.width,original_height=area.height;
        if(draw.reticle && controls_.crosshair_percent!=100) if(const auto center=gx::draw_center_ndc(draw)) gx::scale_viewport_about(*center,std::min(controls_.crosshair_percent,100u)/100.f,area.x,area.y,area.width,area.height);
        if(draw.late_aim && controls_.aim && area.width>0 && area.height>0 && !(draw.wake && controls_.aim_history && controls_.wake_seconds>0)) { const auto aim=controls_.aim(); const auto& from=draw.aim_exact ? draw.aim_ref : packet.aim_consumed; push.shift[0]=(aim[0]-from[0])*gx::aim_to_ndc_x()*original_width/area.width; push.shift[1]=(aim[1]-from[1])*gx::aim_to_ndc_y()*original_height/area.height; if(push.shift[0]!=0 || push.shift[1]!=0) ++stats_.late_aim_draws; }
        if(area.width>0 && area.height>0) {
            if(push.motion!=UINT32_MAX) { auto& motion=motions[push.motion]; if(motion.kind==1) { std::array<float,2> shift{}; native_viewport(area,motion.previous,shift); } else motion.previous=native_camera_motion(area,motion.previous); }
            const auto adjusted=native_viewport(area,push.projection,push.shift); area=adjusted.viewport; info.viewport_clip=adjusted.clip;
        }
        push.viewport[0]=area.x; push.viewport[1]=area.y; push.viewport[2]=area.width; push.viewport[3]=area.height;
        if(info.alpha_constant!=UINT_MAX) { constants[info.alpha_constant].push=push; constants[info.alpha_constant].push.alpha_pass=1; }
    }
    if(scene_packet && shadow_fit_ && controls_.effects.shadows!=EffectLevel::off) for(auto& info:draws) {
        const auto& draw=*info.draw; const auto summary=gx::summarize(draw);
        const bool triangles=draw.primitive==0x80 || draw.primitive==0x90 || draw.primitive==0x98 || draw.primitive==0xa0;
        const uint32_t alpha=draw.uniforms->pixel[0];
        if(!draw.mesh || !info.vertices || !info.count || !summary.perspective || !summary.z_write || !triangles || ((alpha>>16)&7)!=7 || ((alpha>>19)&7)!=7) continue;
        ShadowPush shadow{}; const auto& main=constants[info.constant].push;
        shadow.push.projection=shadow_fit_->view_to_light; shadow.push.palette=main.palette; shadow.push.current=main.current; shadow.push.indexed=main.indexed; shadow.push.bank=main.bank;
        shadow.push.viewport[2]=shadow.push.viewport[3]=float(shadow_preset(controls_.effects.shadows).resolution);
        info.shadow_constant=UINT(shadow_pushes.size()); shadow_pushes.push_back(shadow);
    }
    upload(slot.constants,constants.data(),constants.size()*sizeof(PackedDraw),D3D11_BIND_CONSTANT_BUFFER);
    upload(slot.shadows,shadow_pushes.data(),shadow_pushes.size()*sizeof(ShadowPush),D3D11_BIND_CONSTANT_BUFFER);
    if(scene_packet) { if(motions.empty()) motions.emplace_back(); upload(slot.motions,motions.data(),motions.size()*sizeof(MotionBlock),D3D11_BIND_SHADER_RESOURCE,true,sizeof(MotionBlock)); }
    context_->Begin(slot.disjoint.Get()); context_->End(slot.start.Get()); unbind_images();
    if(overlay_frame) {
        if(slot.output.width!=width_ || slot.output.height!=height_) slot.output=image(width_,height_);
        context_->CopyResource(slot.output.texture.Get(),held_.texture.Get()); context_->ClearDepthStencilView(held_depth_view_.Get(),D3D11_CLEAR_DEPTH,1,0);
    } else if(!efb_initialized_) { float color[4]; for(size_t i=0;i<4;++i)color[i]=packet.clear_color[i]/255.f; context_->ClearRenderTargetView(efb_.rtv.Get(),color); context_->ClearDepthStencilView(depth_view_.Get(),D3D11_CLEAR_DEPTH,packet.clear_depth/float(0xffffff),0); efb_initialized_=true; }
    if(scene_packet) {
        if(!scene_initialized_) { float color[4]; for(size_t i=0;i<4;++i) color[i]=packet.clear_color[i]/255.f; context_->ClearRenderTargetView(scene_color_.rtv.Get(),color); context_->ClearDepthStencilView(scene_depth_view_.Get(),D3D11_CLEAR_DEPTH,packet.clear_depth/float(0xffffff),0); scene_initialized_=true; }
        if(scene_clear_) { gx::FramePacket clear; clear.clear_color=scene_clear_->color; clear.clear_depth=scene_clear_->depth; clear_region(clear,scene_clear_->region,true); scene_clear_.reset(); }
        if(!scene_cleared_) { const float zero[4]{}; context_->ClearRenderTargetView(scene_motion_.rtv.Get(),zero); context_->ClearRenderTargetView(scene_gbuffer_.rtv.Get(),zero); scene_cleared_=true; }
    }
    auto& draw_target=overlay_frame ? slot.output : scene_packet ? scene_color_ : efb_;
    auto* draw_depth=overlay_frame ? held_depth_view_.Get() : scene_packet ? scene_depth_view_.Get() : depth_view_.Get();
    context_->IASetInputLayout(layout_.Get()); context_->VSSetShader(scene_packet ? motion_vertex_.Get() : vertex_.Get(),nullptr,0);
    ID3D11ShaderResourceView* structured[]{slot.palettes.srv.Get(),slot.banks.srv.Get(),scene_packet ? slot.motions.srv.Get() : nullptr}; context_->VSSetShaderResources(8,3,structured);
    // What the context has bound across this loop's draws: calls that would repeat it are skipped.
    D3D11_VIEWPORT bound_viewport{}; D3D11_RECT bound_scissor{}; bool rasterizer_bound=false,images_bound=false;
    D3D11_PRIMITIVE_TOPOLOGY bound_topology=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED; ID3D11Buffer* bound_vertices=nullptr; ID3D11Buffer* bound_indices=nullptr;
    std::array<ID3D11ShaderResourceView*,8> bound_images{}; std::array<ID3D11SamplerState*,8> bound_samplers{};
    ID3D11RenderTargetView* bound_targets[3]{}; UINT bound_target_count=UINT_MAX; ID3D11PixelShader* bound_pixel=nullptr;
    for(const auto& info:draws) {
        const auto& draw=*info.draw; const auto& push=constants[info.constant].push;
        if(draw.state->cull==3) continue;
        const auto v=draw.state->viewport.value_or(std::array<float,6>{0,0,640,480,0,1}); if(push.viewport[2]<=0 || push.viewport[3]<=0) continue;
        D3D11_VIEWPORT viewport{push.viewport[0],push.viewport[1],push.viewport[2],push.viewport[3],v[4],v[5]};
        if(!rasterizer_bound || std::memcmp(&bound_viewport,&viewport,sizeof viewport)) { context_->RSSetViewports(1,&viewport); bound_viewport=viewport; }
        D3D11_RECT scissor{LONG(std::clamp<int64_t>(info.viewport_clip[0],0,target_width)),LONG(std::clamp<int64_t>(info.viewport_clip[1],0,target_height)),LONG(std::clamp<int64_t>(info.viewport_clip[2],0,target_width)),LONG(std::clamp<int64_t>(info.viewport_clip[3],0,target_height))};
        if(draw.state->scissor && !overlay_frame && push.shift[0]==0 && push.shift[1]==0) { const auto& s=*draw.state->scissor; scissor.left=std::max(scissor.left,LONG(std::clamp<int64_t>(int64_t(s[0])*target_width/640,0,target_width))); scissor.top=std::max(scissor.top,LONG(std::clamp<int64_t>(int64_t(s[1])*target_height/480,0,target_height))); scissor.right=std::min(scissor.right,LONG(std::clamp<int64_t>((int64_t(s[0])+s[2])*target_width/640,0,target_width))); scissor.bottom=std::min(scissor.bottom,LONG(std::clamp<int64_t>((int64_t(s[1])+s[3])*target_height/480,0,target_height))); }
        scissor.right=std::max(scissor.left,scissor.right); scissor.bottom=std::max(scissor.top,scissor.bottom);
        if(!rasterizer_bound || std::memcmp(&bound_scissor,&scissor,sizeof scissor)) { context_->RSSetScissorRects(1,&scissor); bound_scissor=scissor; }
        rasterizer_bound=true;
        D3D11_PRIMITIVE_TOPOLOGY topology;
        switch(draw.primitive) { case 0x80:case 0x90:case 0x98:case 0xa0:topology=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;break; case 0xa8:case 0xb0:topology=D3D11_PRIMITIVE_TOPOLOGY_LINELIST;break; default:throw std::invalid_argument("unsupported GX primitive"); }
        if(topology!=bound_topology) { context_->IASetPrimitiveTopology(topology); bound_topology=topology; }
        ID3D11Buffer* vertex=info.vertices ? info.vertices : slot.vertices.buffer.Get(); ID3D11Buffer* index_buffer=info.indices ? info.indices : slot.indices.buffer.Get();
        if(vertex!=bound_vertices) { UINT stride=sizeof(gx::GpuMeshVertex),offset=0; context_->IASetVertexBuffers(0,1,&vertex,&stride,&offset); bound_vertices=vertex; }
        if(index_buffer!=bound_indices) { context_->IASetIndexBuffer(index_buffer,DXGI_FORMAT_R32_UINT,0); bound_indices=index_buffer; }
        if(!images_bound || bound_images!=info.images) { context_->PSSetShaderResources(0,8,info.images.data()); bound_images=info.images; }
        if(!images_bound || bound_samplers!=info.samplers) { context_->PSSetSamplers(0,8,info.samplers.data()); bound_samplers=info.samplers; }
        images_bound=true;
        const auto issue=[&](UINT index,bool alpha,UINT first,UINT count,bool surface_only=false) {
            const bool logic=draw.blend.mode==2 && !alpha && !surface_only;
            const bool mrt=scene_packet && !logic;
            state(draw,alpha,mrt,surface_only);
            ID3D11RenderTargetView* targets[]{logic ? draw_target.logic_rtv.Get() : draw_target.rtv.Get(),mrt ? scene_motion_.rtv.Get() : nullptr,mrt ? scene_gbuffer_.rtv.Get() : nullptr};
            const UINT target_count=mrt ? 3 : 1;
            if(target_count!=bound_target_count || !std::equal(targets,targets+target_count,bound_targets)) { context_->OMSetRenderTargets(target_count,targets,draw_depth); std::copy(targets,targets+target_count,bound_targets); bound_target_count=target_count; images_bound=false; }   // binding targets drops views of the same resources: bind the images again
            ID3D11PixelShader* pixel=scene_packet ? (logic ? motion_logic_pixel_.Get() : motion_pixel_.Get()) : (logic ? logic_pixel_.Get() : pixel_.Get());
            if(!logic && tev_) {   // the draw's own TEV setup as constants, once it is compiled
                TevWords words; std::copy_n(constants[index].tev.begin(),66,words.begin()); words[66]=alpha ? 1u : 0u;
                if(const auto* entry=tev_->get(scene_packet ? 1 : 0,words); entry && entry->object) pixel=static_cast<ComPtr<ID3D11PixelShader>*>(entry->object.get())->Get();
            }
            if(pixel!=bound_pixel) { context_->PSSetShader(pixel,nullptr,0); bound_pixel=pixel; }
            ID3D11Buffer* buffers[]{slot.constants.buffer.Get(),slot.constants.buffer.Get()}; UINT starts[]{index*160,index*160+144},counts[]{144,16}; context1_->VSSetConstantBuffers1(0,2,buffers,starts,counts); context1_->PSSetConstantBuffers1(0,2,buffers,starts,counts);
            context_->DrawIndexed(count,info.first_index+first,INT(info.first_vertex));
        };
        if(scene_packet && draw.blend.mode==2 && (draw.uniforms->pixel[1]&17)==17) {
            // Logic operations cannot share independent float MRT state. Preserve primitive depth ordering.
            const UINT primitive_size=draw.primitive==0xa8 || draw.primitive==0xb0 ? 2 : 3;
            for(UINT first=0;first+primitive_size<=info.count;first+=primitive_size) {
                issue(info.constant,false,first,primitive_size,true);
                issue(info.constant,false,first,primitive_size);
            }
        } else issue(info.constant,false,0,info.count);
        if(info.alpha_constant!=UINT_MAX) issue(info.alpha_constant,true,0,info.count);
    }
    unbind_images();
    if(!shadow_pushes.empty()) {
        ensure_shadow();
        if(!shadow_cleared_) context_->ClearDepthStencilView(shadow_view_.Get(),D3D11_CLEAR_DEPTH,1,0);
        context_->OMSetRenderTargets(0,nullptr,shadow_view_.Get()); context_->OMSetDepthStencilState(shadow_depth_.Get(),0); context_->OMSetBlendState(nullptr,nullptr,UINT_MAX);
        last_blend_=nullptr; last_depth_=nullptr; last_raster_=nullptr;   // the shadow pass sets its own
        context_->RSSetState(shadow_raster_.Get()); const float size=float(shadow_map_.width); const D3D11_VIEWPORT viewport{0,0,size,size,0,1}; const D3D11_RECT scissor{0,0,LONG(shadow_map_.width),LONG(shadow_map_.height)};
        context_->RSSetViewports(1,&viewport); context_->RSSetScissorRects(1,&scissor); context_->IASetInputLayout(shadow_layout_.Get()); context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(shadow_vertex_.Get(),nullptr,0); context_->PSSetShader(nullptr,nullptr,0);
        for(const auto& info:draws) if(info.shadow_constant!=UINT_MAX) {
            UINT stride=sizeof(gx::GpuMeshVertex),offset=0; context_->IASetVertexBuffers(0,1,&info.vertices,&stride,&offset); context_->IASetIndexBuffer(info.indices,DXGI_FORMAT_R32_UINT,0);
            const UINT first=info.shadow_constant*16,count=16; context1_->VSSetConstantBuffers1(1,1,slot.shadows.buffer.GetAddressOf(),&first,&count); context_->DrawIndexed(info.count,info.first_index,INT(info.first_vertex));
        }
        unbind_images(); shadow_cleared_=true;
    }
    if(scene_end) {
        if(scene_packet) {
            tracker_->end_frame(); scene_tracking_=false; scene_captured_=true;
            if(controls_.effects.contact_shadows || controls_.effects.shadows!=EffectLevel::off) { auto light=gx::select_dominant(scene_lights_.lights(),dominant_light_); if(light_changed(shadow_light_,light)) shadow_cleared_=false; dominant_light_=std::move(light); }
            if(scene_projection_ && (controls_.effects.ambient_occlusion!=EffectLevel::off || controls_.effects.contact_shadows || shadow_cleared_)) occlude_scene(next_boundary);
        }
        bloom_valid_=false;
        if(scene_packet) reconstruct_scene(next_boundary,packet.black_output);
        if(scene_packet) { if(blur_history_valid_ && !tracker_->camera_cut() && !packet.black_output && controls_.effects.motion_blur!=EffectLevel::off) blur_scene(next_boundary); blur_history_valid_=!packet.black_output; }
        if(bloom_mix(bloom_,hdr_active_)>0 && !packet.black_output) bloom_scene(next_boundary);
        if(restore_copy) {
            auto& saved=texture(restore_copy->target);
            blit(efb_,saved.image,restore_copy->source,uint32_t(restore_copy->target->channels));
            saved.initialized=true; slot.textures.push_back(restore_copy->target); restore_copy.reset();
        }
    }
    if(copied) { blit(scene_packet && !scene_end ? scene_color_ : efb_,copied->image,packet.texture_copy->source,uint32_t(packet.texture_copy->target->channels)); copied->initialized=true; if(packet.texture_copy->target->guest_address) latest_[packet.texture_copy->target->guest_address]=packet.texture_copy->target; }
    if(scene_phase && !scene_end) {
        if(copied && !packet.clear_after_copy) restore_copy=packet.texture_copy;
        else if(!packet.draws.empty() || (copied && packet.clear_after_copy)) restore_copy.reset();
    }
    if(packet.present && display) {
        if(slot.output.width!=width_ || slot.output.height!=height_) slot.output=image(width_,height_);
        if(!overlay_frame) blit(packet.black_output ? black_ : efb_,slot.output,packet.black_output ? std::array<uint32_t,4>{0,0,640,480} : packet.display_source,0,false);
        const bool has_ui=!hold && (menu_build_ || (!packet.black_output && !packet.prompts.empty()));
        if(hdr_active_) {
            if(has_ui) { if(slot.ui.width!=width_ || slot.ui.height!=height_) slot.ui=image(width_,height_); const float transparent[4]{}; context_->ClearRenderTargetView(slot.ui.rtv.Get(),transparent); draw_overlay(packet,slot.ui); }
            encode_hdr(packet,slot.output,has_ui ? &slot.ui : nullptr);
        } else {
            if(has_ui) draw_overlay(packet,slot.output);
            unbind_images(); context_->CopyResource(hold ? held_.texture.Get() : backbuffer_.texture.Get(),slot.output.texture.Get());
        }
        slot.copied=true;
    }
    if(!overlay_frame && packet.clear_after_copy && (packet.present || copied || packet.clear_only)) {
        const auto region=copied ? packet.texture_copy->source : packet.display_source; clear_region(packet,region,scene_packet && !scene_end);
        if(packet.present) scene_clear_=SceneClear{region,packet.clear_color,packet.clear_depth};
    }
    context_->End(slot.stop.Get()); context_->End(slot.disjoint.Get()); context_->End(slot.event.Get());
    slot.pending=true; slot.serial=serial; last_serial_=serial; next_slot_=(next_slot_+1)%slots_.size();
    if(!overlay_frame) {
        boundary_=std::move(next_boundary); frame_start_=packet.present; scene_phase_=scene_phase && !scene_end; scene_restore_copy_=std::move(restore_copy);
        if(layer_active) for(const auto& draw:packet.draws) if(draw.layer) {
            auto& saved=frame_reticle_.emplace_back(draw);
            if(saved.late_aim && !saved.aim_exact) { saved.aim_exact=true; saved.aim_ref=packet.aim_consumed; }
        }
        if(hold) { held_draws_=std::move(frame_reticle_); held_prompts_=packet.prompts; held_source_=packet.display_source; held_aim_=packet.aim_consumed; held_valid_=true; held_presented_=false; }
        else if(packet.present && display && packet.black_output) invalidate_held();
        if(packet.present) frame_reticle_.clear();
        if(packet.present) ++frame_index_;
    }
    stats_.submit_ms=std::chrono::duration<double,std::milli>(Clock::now()-started).count();
    if(slot.copied && !hold) { const auto before=Clock::now(); BOOL fullscreen=FALSE; check(swapchain_->GetFullscreenState(&fullscreen,nullptr),"DXGI fullscreen state"); check(swapchain_->Present(0,tearing_ && !fullscreen ? DXGI_PRESENT_ALLOW_TEARING : 0),"D3D11 present"); stats_.present_ms=std::chrono::duration<double,std::milli>(Clock::now()-before).count(); ++stats_.presentations; if(overlay_frame) held_presented_=true; }
    else context_->Flush();
    if((serial&63)==0) sweep(); return true;
}
void D3D11Renderer::collect() {
    if(!context_) return;
    for(auto& slot:slots_) if(slot.pending) {
        const HRESULT status=context_->GetData(slot.event.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH); check(status,"GPU completion query"); if(status==S_FALSE) continue;
        slot.pending=false; completed_.push_back(slot.serial);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{}; UINT64 start=0,stop=0;
        if(context_->GetData(slot.disjoint.Get(),&clock,sizeof clock,D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK && !clock.Disjoint && clock.Frequency && context_->GetData(slot.start.Get(),&start,sizeof start,D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK && context_->GetData(slot.stop.Get(),&stop,sizeof stop,D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK && stop>=start) stats_.gpu_busy_ns+=uint64_t(double(stop-start)*1e9/double(clock.Frequency));
    }
    check(device_->GetDeviceRemovedReason(),"D3D11 device health");
}
std::vector<uint64_t> D3D11Renderer::poll_completions() { owner(); collect(); auto result=std::move(completed_); completed_.clear(); std::sort(result.begin(),result.end()); return result; }
bool D3D11Renderer::wait_for_pending(uint64_t timeout_ns) {
    owner(); collect(); if(std::none_of(slots_.begin(),slots_.end(),[](const auto& s){return s.pending;})) return false;
    context_->Flush(); const auto deadline=Clock::now()+std::chrono::nanoseconds(std::min(timeout_ns,uint64_t(INT64_MAX)));
    const auto completed=completed_.size(); do { collect(); if(completed_.size()!=completed) return true; std::this_thread::yield(); } while(Clock::now()<deadline); return false;
}
bool D3D11Renderer::copy_ready(const std::shared_ptr<const gx::TexturePixels>& source,bool previous_frame) {
    owner(); if(!source || !source->gpu_only || source->levels.empty()) throw std::invalid_argument("invalid GX copy query");
    if(lost_.contains(source.get())) return true;
    if(const auto found=textures_.find(source.get());found!=textures_.end() && !found->second.owner.expired() && found->second.initialized) return true;
    if(const auto found=aliases_.find(source.get());found!=aliases_.end()) { if(!found->second.requested.expired()) return true; aliases_.erase(found); }
    if(previous_frame && source->guest_address) {
        std::shared_ptr<const gx::TexturePixels> previous;
        if(const auto found=latest_.find(source->guest_address);found!=latest_.end()) previous=found->second.lock();
        if(previous && (previous->channels!=source->channels || previous->levels[0].width!=source->levels[0].width || previous->levels[0].height!=source->levels[0].height)) previous.reset();
        aliases_.emplace(source.get(),Alias{source,std::move(previous)}); return true;
    }
    return false;
}
std::vector<const gx::TexturePixels*> D3D11Renderer::copy_keys() const {
    owner(); std::vector<const gx::TexturePixels*> keys(lost_.begin(),lost_.end());
    for(const auto& [key,entry]:textures_) if(const auto source=entry.owner.lock();source && source->gpu_only && entry.initialized)keys.push_back(key);
    for(const auto& [key,alias]:aliases_) if(!alias.requested.expired()) keys.push_back(key); return keys;
}
void D3D11Renderer::sweep() {
    size_t bytes=0; for(const auto& image:recycled_copies_) bytes+=size_t(image.width)*image.height*16/3;
    const double factor=std::max(efb_.width/640.0,efb_.height/480.0); const size_t budget=size_t(std::min(512.0*1024*1024,64.0*1024*1024*factor*factor));
    for(auto it=textures_.begin();it!=textures_.end();) {
        if(!it->second.owner.expired()) { ++it; continue; }
        const size_t size=size_t(it->second.image.width)*it->second.image.height*16/3;
        if(it->second.gpu_only && recycled_copies_.size()<32 && bytes+size<=budget) { recycled_copies_.push_back(std::move(it->second.image)); bytes+=size; }
        it=textures_.erase(it);
    }
    std::erase_if(meshes_,[](const auto& entry){return entry.second.owner.expired();});
    std::erase_if(aliases_,[](const auto& entry){return entry.second.requested.expired();}); std::erase_if(latest_,[](const auto& entry){return entry.second.expired();});
}
RenderPixels D3D11Renderer::capture(uint64_t serial,bool efb) {
    owner(); if(efb && serial!=last_serial_) throw std::invalid_argument("EFB capture requires latest serial");
    const auto found=std::find_if(slots_.begin(),slots_.end(),[&](const auto& slot){return slot.serial==serial && (efb || slot.copied);}); if(found==slots_.end()) throw std::invalid_argument("capture serial no longer retained");
    drain(); const auto& source=efb ? efb_ : found->output;
    D3D11_TEXTURE2D_DESC desc{}; source.texture->GetDesc(&desc); desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0;
    ComPtr<ID3D11Texture2D> staging; check(device_->CreateTexture2D(&desc,nullptr,&staging),"capture staging texture"); context_->CopyResource(staging.Get(),source.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{}; check(context_->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"capture GPU readback");
    RenderPixels result{serial,source.width,source.height,std::vector<uint8_t>(size_t(source.width)*source.height*4)};
    for(uint32_t y=0;y<source.height;++y) std::memcpy(result.rgba.data()+size_t(y)*source.width*4,static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(source.width)*4);
    context_->Unmap(staging.Get(),0); collect(); return result;
}
RenderPixels D3D11Renderer::capture_scene() {
    owner(); if(!scene_captured_) throw std::logic_error("no completed scene target to capture");
    drain(); D3D11_TEXTURE2D_DESC desc{}; scene_color_.texture->GetDesc(&desc); desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0;
    ComPtr<ID3D11Texture2D> staging; check(device_->CreateTexture2D(&desc,nullptr,&staging),"scene capture staging"); context_->CopyResource(staging.Get(),scene_color_.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{}; check(context_->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"scene capture readback");
    RenderPixels result{last_serial_,scene_color_.width,scene_color_.height,std::vector<uint8_t>(size_t(scene_color_.width)*scene_color_.height*4)};
    for(uint32_t y=0;y<result.height;++y) std::memcpy(result.rgba.data()+size_t(y)*result.width*4,static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(result.width)*4);
    context_->Unmap(staging.Get(),0); return result;
}
MotionPixels D3D11Renderer::capture_motion() {
    owner(); if(!scene_cleared_ || !scene_motion_.texture) throw std::logic_error("no motion target to capture");
    drain(); D3D11_TEXTURE2D_DESC desc{}; scene_motion_.texture->GetDesc(&desc); desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0;
    ComPtr<ID3D11Texture2D> staging; check(device_->CreateTexture2D(&desc,nullptr,&staging),"motion capture staging"); context_->CopyResource(staging.Get(),scene_motion_.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{}; check(context_->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"motion capture readback");
    MotionPixels result{scene_motion_.width,scene_motion_.height,std::vector<float>(size_t(scene_motion_.width)*scene_motion_.height*2)};
    const auto half=[](uint16_t h) {
        const uint32_t sign=uint32_t(h>>15)<<31,exponent=(h>>10)&31,mantissa=h&1023;
        if(!exponent) return std::bit_cast<float>(sign)+(sign ? -1.f : 1.f)*std::ldexp(float(mantissa),-24);
        if(exponent==31) return std::bit_cast<float>(sign|0x7f800000u|(mantissa<<13));
        return std::bit_cast<float>(sign|((exponent+112)<<23)|(mantissa<<13));
    };
    for(uint32_t y=0;y<result.height;++y) {
        const auto* row=reinterpret_cast<const uint16_t*>(static_cast<const std::byte*>(mapped.pData)+size_t(y)*mapped.RowPitch);
        for(uint32_t x=0;x<result.width;++x) { const size_t at=(size_t(y)*result.width+x)*2; result.rg[at]=half(row[x*4]); result.rg[at+1]=half(row[x*4+1]); }
    }
    context_->Unmap(staging.Get(),0); return result;
}
NativeStats D3D11Renderer::stats() const {
    owner(); auto result=stats_; const UINT64 messages=info_ ? info_->GetNumStoredMessagesAllowedByRetrievalFilter() : 0; for(UINT64 i=0;i<messages;++i) {
        SIZE_T size=0; if(FAILED(info_->GetMessage(i,nullptr,&size))) continue; std::vector<std::byte> bytes(size); auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
        if(SUCCEEDED(info_->GetMessage(i,message,&size)) && (message->Severity==D3D11_MESSAGE_SEVERITY_ERROR || message->Severity==D3D11_MESSAGE_SEVERITY_CORRUPTION)) {
            ++result.validation_errors;
            if(i>=reported_messages_) std::fprintf(stderr,"D3D11 validation [%u]: %s\n",unsigned(message->ID),message->pDescription);
        }
    } reported_messages_=messages; return result;
}
void D3D11Renderer::shutdown() {
    if(native_upscaler_) { try { drain(); } catch(const std::exception& error) { std::fprintf(stderr,"D3D11 DLSS shutdown drain: %s\n",error.what()); } native_upscaler_.reset(); }
    shutdown_overlay();
    // ClearState releases context-held resource references even after device removal.
    if(context_) { context_->ClearState(); context_->Flush(); }
    if(swapchain_) swapchain_->SetFullscreenState(FALSE,nullptr);
    for(auto& slot:slots_)slot={}; textures_.clear(); recycled_copies_.clear(); aliases_.clear(); latest_.clear(); meshes_.clear(); lost_.clear(); blends_.clear(); depths_.clear(); rasters_.clear(); samplers_.clear();
    white_={}; black_={}; efb_={}; backbuffer_={}; clear_constants_={}; clear_vertices_={}; clear_indices_={}; clear_palette_={}; blit_constants_={};
    invalidate_held(); held_={}; held_depth_view_.Reset(); held_depth_.Reset();
    for(auto& level:bloom_down_) level={}; for(auto& level:bloom_up_) level={}; bloom_color_={}; bloom_region_={}; bloom_constants_={}; bloom_sampler_.Reset();
    for(auto& shader:bloom_shaders_) shader.Reset(); boundary_.reset(); scene_restore_copy_.reset(); frame_start_=true; scene_phase_=false;
    hdr_constants_={}; hdr_vertex_.Reset(); hdr_pixel_.Reset(); hdr_active_=false; hdr_metadata_dirty_=true; bloom_valid_=false;
    smaa_source_={}; smaa_edges_={}; smaa_weights_={}; smaa_output_={}; smaa_area_={}; smaa_search_={}; smaa_constants_={}; rcas_constants_={};
    for(auto& shader:smaa_shaders_) shader.Reset(); rcas_shader_.Reset();
    scene_color_={}; scene_depth_={}; scene_motion_={}; scene_gbuffer_={}; tracker_.reset(); scene_projection_.reset(); scene_captured_=scene_tracking_=scene_cleared_=false; motion_vertex_.Reset(); motion_pixel_.Reset();
    ao_raw_={}; ao_filtered_={}; contact_={}; ao_color_={}; blur_tiles_={}; effect_color_={}; occlusion_constants_={}; occlusion_shadow_={}; blur_constants_={}; scene_lights_.clear(); dominant_light_.reset(); blur_history_valid_=false;
    for(auto& shader:occlusion_shaders_) shader.Reset(); for(auto& shader:blur_shaders_) shader.Reset();
    shadow_map_={}; shadow_view_.Reset(); shadow_depth_.Reset(); shadow_raster_.Reset(); shadow_vertex_.Reset(); shadow_layout_.Reset(); shadow_fit_.reset(); shadow_light_.reset(); shadow_cleared_=false;
    scene_depth_view_.Reset(); scene_initialized_=false; scene_clear_.reset(); depth_vertex_.Reset(); depth_pixel_.Reset(); depth_constants_={};
    tev_.reset(); depth_view_.Reset(); depth_.Reset(); layout_.Reset(); pixel_.Reset(); logic_pixel_.Reset(); motion_logic_pixel_.Reset(); vertex_.Reset(); blit_pixel_.Reset(); blit_vertex_.Reset(); info_.Reset(); reported_messages_=0; swapchain_.Reset(); context1_.Reset(); context_.Reset();
    if(streamline_) { streamline_->shutdown(); streamline_.reset(); } dlss_available_=false;   // after the swapchain and the context, before the device (as on DirectX 12)
    device1_.Reset(); device_.Reset(); initialized_=false;
}

void D3D11Renderer::reset_scene() {
    if(native_upscaler_) { drain(); context_->ClearState(); native_upscaler_.reset(); }
    if(initialized_ && upscaler_kind_==UpscalerKind::dlss) {
        if(!streamline_ || !dlss_available_) throw std::runtime_error("D3D11 DLSS requires renderer restart with Streamline enabled");
        native_upscaler_=make_native_dlss_upscaler(*streamline_,upscale_mode_);
    }
    scene_color_={}; scene_depth_={}; scene_motion_={}; scene_gbuffer_={};
    scene_depth_view_.Reset(); scene_initialized_=false; scene_clear_.reset(); upscale_reset_=true; jitter_={};
    scene_captured_=scene_tracking_=scene_cleared_=false; scene_projection_.reset(); tracker_=std::make_unique<gx::MotionTracker>();
    ao_raw_={}; ao_filtered_={}; contact_={}; ao_color_={}; blur_tiles_={}; effect_color_={}; scene_lights_.clear(); dominant_light_.reset(); blur_history_valid_=false;
    shadow_map_={}; shadow_view_.Reset(); shadow_fit_.reset(); shadow_light_.reset(); shadow_cleared_=false;
}
void D3D11Renderer::ensure_scene() {
    if(scene_color_.texture && scene_depth_view_) return;
    const RenderExtent output{efb_.width,efb_.height};
    const auto extent=internal_extent(output,uses_jitter(upscaler_kind_) ? upscale_mode_ : UpscaleMode::native,native_upscaler_ ? native_upscaler_->minimum_internal(output) : RenderExtent{1,1});
    if(scene_color_.width==extent.width && scene_color_.height==extent.height && scene_depth_view_) return;
    scene_color_=image(extent.width,extent.height,1,DXGI_FORMAT_R8G8B8A8_UNORM,nullptr,true);
    scene_motion_=image(extent.width,extent.height,1,DXGI_FORMAT_R16G16B16A16_FLOAT);
    scene_gbuffer_=image(extent.width,extent.height,1,DXGI_FORMAT_R16G16B16A16_UNORM);
    scene_depth_={}; scene_depth_view_.Reset(); scene_depth_.width=extent.width; scene_depth_.height=extent.height;
    D3D11_TEXTURE2D_DESC desc{}; depth_->GetDesc(&desc); desc.Width=extent.width; desc.Height=extent.height;
    check(device_->CreateTexture2D(&desc,nullptr,&scene_depth_.texture),"internal scene depth");
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{}; dsv.Format=DXGI_FORMAT_D32_FLOAT; dsv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    check(device_->CreateDepthStencilView(scene_depth_.texture.Get(),&dsv,&scene_depth_view_),"internal scene depth target");
    D3D11_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=DXGI_FORMAT_R32_FLOAT; view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels=1;
    check(device_->CreateShaderResourceView(scene_depth_.texture.Get(),&view,&scene_depth_.srv),"scene depth sample view");
    scene_initialized_=false; upscale_reset_=true;
}
void D3D11Renderer::ensure_shadow() {
    const uint32_t resolution=shadow_preset(controls_.effects.shadows).resolution;
    if(!resolution) throw std::logic_error("shadow map requested while disabled");
    if(shadow_map_.width!=resolution || !shadow_view_) {
        shadow_map_={}; shadow_view_.Reset(); shadow_map_.width=shadow_map_.height=resolution;
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=desc.Height=resolution; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1; desc.Format=DXGI_FORMAT_R32_TYPELESS; desc.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
        check(device_->CreateTexture2D(&desc,nullptr,&shadow_map_.texture),"shadow depth texture");
        D3D11_DEPTH_STENCIL_VIEW_DESC dsv{}; dsv.Format=DXGI_FORMAT_D32_FLOAT; dsv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        check(device_->CreateDepthStencilView(shadow_map_.texture.Get(),&dsv,&shadow_view_),"shadow depth view");
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=DXGI_FORMAT_R32_FLOAT; srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels=1;
        check(device_->CreateShaderResourceView(shadow_map_.texture.Get(),&srv,&shadow_map_.srv),"shadow sample view"); shadow_cleared_=false;
    }
    if(!shadow_depth_) {
        D3D11_DEPTH_STENCIL_DESC depth{}; depth.DepthEnable=TRUE; depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL; depth.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
        check(device_->CreateDepthStencilState(&depth,&shadow_depth_),"shadow depth state"); ++stats_.state_objects;
    }
    if(!shadow_raster_) {
        D3D11_RASTERIZER_DESC raster{}; raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_NONE; raster.DepthClipEnable=TRUE; raster.ScissorEnable=TRUE; raster.DepthBias=2; raster.SlopeScaledDepthBias=2.f;
        check(device_->CreateRasterizerState(&raster,&shadow_raster_),"shadow rasterizer state"); ++stats_.state_objects;
    }
}
void D3D11Renderer::upscale_depth() {
    unbind_images();
    const std::array<uint32_t,4> sizes{scene_depth_.width,scene_depth_.height,efb_.width,efb_.height};
    upload(depth_constants_,sizes.data(),sizeof sizes,D3D11_BIND_CONSTANT_BUFFER);
    gx::DrawPacket draw; draw.uniforms.edit().pixel[1]=31; state(draw,false);
    const D3D11_VIEWPORT viewport{0,0,float(efb_.width),float(efb_.height),0,1}; const D3D11_RECT scissor{0,0,LONG(efb_.width),LONG(efb_.height)};
    context_->RSSetViewports(1,&viewport); context_->RSSetScissorRects(1,&scissor); context_->OMSetRenderTargets(0,nullptr,depth_view_.Get());
    context_->IASetInputLayout(nullptr); context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); context_->VSSetShader(depth_vertex_.Get(),nullptr,0); context_->PSSetShader(depth_pixel_.Get(),nullptr,0);
    context_->PSSetConstantBuffers(0,1,depth_constants_.buffer.GetAddressOf()); context_->PSSetShaderResources(0,1,scene_depth_.srv.GetAddressOf()); context_->Draw(3,0); unbind_images();
}
void D3D11Renderer::reconstruct_scene(const gx::SceneBoundary& boundary,bool black) {
    const auto viewport=boundary.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
    const auto region=[&](const Image& target) { return RenderExtent{uint32_t(std::clamp<long>(std::lround((viewport[0]+viewport[2])*target.width/640),1,target.width)),uint32_t(std::clamp<long>(std::lround((viewport[1]+viewport[3])*target.height/480),1,target.height))}; };
    const auto render_size=region(scene_color_),output_size=region(efb_);
    if(upscaler_kind_==UpscalerKind::dlss) {
        if(!native_upscaler_) throw std::logic_error("DLSS reconstruction is not initialized");
        NativeUpscaleInputs input; input.command=context_.Get();
        input.color={scene_color_.texture.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,0,{scene_color_.width,scene_color_.height}};
        input.depth={scene_depth_.texture.Get(),DXGI_FORMAT_R32_TYPELESS,0,{scene_depth_.width,scene_depth_.height}};
        input.motion={scene_motion_.texture.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,0,{scene_motion_.width,scene_motion_.height}};
        input.output={efb_.texture.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,0,{efb_.width,efb_.height}};
        input.render_size=render_size; input.output_size=output_size; input.jitter=jitter_; input.reset=upscale_reset_ || tracker_->camera_cut() || black;
        input.projection=upscale_projection_; input.clip_to_prev_clip=tracker_->clip_to_prev_clip(); input.sharpening=sharpening_; input.sharpness=float(sharpness_);
        context_->ClearState(); native_upscaler_->evaluate(input); context_->ClearState();
    } else if(upscaler_kind_==UpscalerKind::smaa) antialias_scene(boundary);
    else {
        // Scale only the scene viewport into the corresponding EFB region; HUD remains at output resolution.
        struct Constants { float uv[4]; uint32_t channels,padding[3]; };
        const Constants constants{{0,0,float(render_size.width)/scene_color_.width,float(render_size.height)/scene_color_.height},0,{}};
        upload(blit_constants_,&constants,sizeof constants,D3D11_BIND_CONSTANT_BUFFER); unbind_images(); gx::DrawPacket draw; state(draw,false);
        const D3D11_VIEWPORT target{0,0,float(output_size.width),float(output_size.height),0,1}; const D3D11_RECT scissor{0,0,LONG(output_size.width),LONG(output_size.height)};
        context_->RSSetViewports(1,&target); context_->RSSetScissorRects(1,&scissor); context_->OMSetRenderTargets(1,efb_.rtv.GetAddressOf(),nullptr); context_->IASetInputLayout(nullptr); context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(blit_vertex_.Get(),nullptr,0); context_->PSSetShader(blit_pixel_.Get(),nullptr,0); context_->VSSetConstantBuffers(0,1,blit_constants_.buffer.GetAddressOf()); context_->PSSetConstantBuffers(0,1,blit_constants_.buffer.GetAddressOf());
        context_->PSSetShaderResources(0,1,scene_color_.srv.GetAddressOf()); auto* sample=sampler(upscaler_kind_==UpscalerKind::blit ? 128|16 : 0,false); context_->PSSetSamplers(0,1,&sample); context_->Draw(3,0); unbind_images();
    }
    if(sharpening_ && upscaler_kind_!=UpscalerKind::off && upscaler_kind_!=UpscalerKind::smaa) sharpen_scene(boundary);
    upscale_depth(); upscale_reset_=false; ++upscale_frame_;
}
void D3D11Renderer::sharpen_scene(const gx::SceneBoundary& boundary) {
    const auto viewport=boundary.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
    const uint32_t width=uint32_t(std::clamp<long>(std::lround((viewport[0]+viewport[2])*efb_.width/640),1,efb_.width)),height=uint32_t(std::clamp<long>(std::lround((viewport[1]+viewport[3])*efb_.height/480),1,efb_.height));
    if(effect_color_.width!=efb_.width || effect_color_.height!=efb_.height) effect_color_=image(efb_.width,efb_.height,1,DXGI_FORMAT_R8G8B8A8_UNORM,nullptr,true);
    struct Constants { int32_t size[2]; float sharpness,unused; };
    const Constants constants{{int32_t(width),int32_t(height)},rcas_constant(sharpness_),0}; upload(rcas_constants_,&constants,sizeof constants,D3D11_BIND_CONSTANT_BUFFER); unbind_images();
    context_->CSSetShader(rcas_shader_.Get(),nullptr,0); context_->CSSetConstantBuffers(0,1,rcas_constants_.buffer.GetAddressOf()); context_->CSSetShaderResources(0,1,efb_.srv.GetAddressOf()); context_->CSSetUnorderedAccessViews(0,1,effect_color_.uav.GetAddressOf(),nullptr); context_->Dispatch((width+7)/8,(height+7)/8,1);
    ID3D11ShaderResourceView* none_srv=nullptr; ID3D11UnorderedAccessView* none_uav=nullptr; context_->CSSetShaderResources(0,1,&none_srv); context_->CSSetUnorderedAccessViews(0,1,&none_uav,nullptr); context_->CSSetShader(nullptr,nullptr,0);
    const D3D11_BOX region{0,0,0,width,height,1}; context_->CopySubresourceRegion(efb_.texture.Get(),0,0,0,0,effect_color_.texture.Get(),0,&region);
}

void D3D11Renderer::invalidate_held() {
    held_valid_=held_presented_=false; held_draws_.clear(); held_prompts_.clear(); frame_reticle_.clear(); held_aim_={};
}
void D3D11Renderer::set_reticle_layer(bool enabled) {
    owner(); if(reticle_layer_!=enabled) { reticle_layer_=enabled; invalidate_held(); }
}
void D3D11Renderer::ensure_held_frame() {
    if(held_.width==width_ && held_.height==height_ && held_depth_view_) return;
    held_=image(width_,height_); held_depth_view_.Reset(); held_depth_.Reset();
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=width_; desc.Height=height_; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1; desc.Format=DXGI_FORMAT_D32_FLOAT; desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    check(device_->CreateTexture2D(&desc,nullptr,&held_depth_),"held-frame depth"); check(device_->CreateDepthStencilView(held_depth_.Get(),nullptr,&held_depth_view_),"held-frame depth view");
    held_valid_=held_presented_=false;
}
bool D3D11Renderer::render_overlay(uint64_t serial) {
    owner(); if(!reticle_layer_ready()) return false;
    collect();
    // Overlay refreshes leave two retired slots available for the next guest packets.
    for(size_t i=0;i<2;++i) if(slots_[(next_slot_+i)%slots_.size()].pending) return false;
    gx::FramePacket packet; packet.serial=serial; packet.present=true; packet.clear_after_copy=false; packet.overlay_only=true; packet.display_source=held_source_; packet.aim_consumed=held_aim_; packet.prompts=held_prompts_;
    for(const auto& draw:held_draws_) packet.draws.push_back(draw);
    return render(packet,true,serial);
}

void D3D11Renderer::occlude_scene(const gx::SceneBoundary& boundary) {
    const auto preset=ao_preset(controls_.effects.ambient_occlusion);
    const bool contact=controls_.effects.contact_shadows && dominant_light_.has_value();
    const bool mapped_shadow=controls_.effects.shadows!=EffectLevel::off && shadow_cleared_ && shadow_fit_ && shadow_light_;
    if(!preset.slices && !contact && !mapped_shadow) return;
    const auto viewport=boundary.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
    const uint32_t width=uint32_t(std::clamp<long>(std::lround((viewport[0]+viewport[2])*scene_color_.width/640),1,scene_color_.width));
    const uint32_t height=uint32_t(std::clamp<long>(std::lround((viewport[1]+viewport[3])*scene_color_.height/480),1,scene_color_.height));
    const uint32_t half_width=(width+1)/2,half_height=(height+1)/2;
    const uint32_t ao_width=preset.full_resolution ? width : half_width,ao_height=preset.full_resolution ? height : half_height;
    if(ao_raw_.width!=ao_width || ao_raw_.height!=ao_height) { ao_raw_=image(ao_width,ao_height,1,DXGI_FORMAT_R8_UNORM,nullptr,true); ao_filtered_=image(ao_width,ao_height,1,DXGI_FORMAT_R8_UNORM,nullptr,true); }
    if(contact_.width!=half_width || contact_.height!=half_height) contact_=image(half_width,half_height,1,DXGI_FORMAT_R8_UNORM,nullptr,true);
    if(ao_color_.width!=scene_color_.width || ao_color_.height!=scene_color_.height) ao_color_=image(scene_color_.width,scene_color_.height,1,DXGI_FORMAT_R8G8B8A8_UNORM,nullptr,true);
    NativeOcclusionParams params{};
    params.viewport[0]=viewport[0]*scene_color_.width/640; params.viewport[1]=viewport[1]*scene_color_.height/480; params.viewport[2]=viewport[2]*scene_color_.width/640; params.viewport[3]=viewport[3]*scene_color_.height/480;
    const auto view=view_params(*scene_projection_);
    params.view_a[0]=view.x_scale; params.view_a[1]=view.y_scale; params.view_a[2]=view.x_offset; params.view_a[3]=view.y_offset; params.view_b[0]=view.depth_a; params.view_b[1]=view.depth_b;
    params.region[0]=width; params.region[1]=height; params.ao_size[0]=ao_width; params.ao_size[1]=ao_height; params.slices=preset.slices; params.steps=preset.steps; params.noise=upscaler_kind_==UpscalerKind::dlss ? upscale_frame_%8+1 : 0;
    params.flags=(preset.slices ? 1u : 0u)|(contact ? 2u : 0u)|(mapped_shadow ? 4u : 0u); params.depth_range[0]=viewport[4]; params.depth_range[1]=viewport[5]; params.contact_size[0]=half_width; params.contact_size[1]=half_height;
    if(contact) { for(size_t i=0;i<3;++i) params.light[i]=dominant_light_->position[i]; params.light[3]=dominant_light_->kind==gx::LightKind::spot ? 1.f : 0.f; }
    NativeShadowBlock shadow{};
    if(mapped_shadow) {
        for(size_t row=0;row<3;++row) std::copy(shadow_fit_->view_to_light[row].begin(),shadow_fit_->view_to_light[row].end(),shadow.rows[row]);
        const auto settings=shadow_preset(controls_.effects.shadows);
        shadow.info[0]=float(settings.resolution); shadow.info[1]=shadow_fit_->texel_world; shadow.info[2]=shadow_fit_->depth_range; shadow.info[3]=0.04f;
        shadow.taps[0]=settings.filter_taps; shadow.taps[1]=settings.blocker_taps; shadow.taps[2]=settings.soft ? 1 : 0; shadow.taps[3]=1;
        const auto& p=shadow_light_->position; const float length=std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]); for(size_t i=0;i<3;++i) shadow.to_light[i]=p[i]/length;
    }
    upload(occlusion_constants_,&params,sizeof params,D3D11_BIND_CONSTANT_BUFFER); upload(occlusion_shadow_,&shadow,sizeof shadow,D3D11_BIND_CONSTANT_BUFFER); unbind_images();
    ID3D11Buffer* constants[]{occlusion_constants_.buffer.Get(),occlusion_shadow_.buffer.Get()}; context_->CSSetConstantBuffers(0,2,constants); context_->CSSetSamplers(0,1,bloom_sampler_.GetAddressOf());
    const auto dispatch=[&](size_t pass,uint32_t w,uint32_t h) {
        ID3D11ShaderResourceView* inputs[]{scene_depth_.srv.Get(),scene_gbuffer_.srv.Get(),pass==1 ? ao_raw_.srv.Get() : nullptr,pass==3 && preset.slices ? ao_filtered_.srv.Get() : nullptr,mapped_shadow ? shadow_map_.srv.Get() : scene_depth_.srv.Get(),pass==3 && contact ? contact_.srv.Get() : nullptr,pass==3 ? scene_color_.srv.Get() : nullptr};
        ID3D11UnorderedAccessView* outputs[]{pass==3 ? ao_color_.uav.Get() : nullptr,pass==0 ? ao_raw_.uav.Get() : nullptr,pass==1 ? ao_filtered_.uav.Get() : nullptr,pass==2 ? contact_.uav.Get() : nullptr};
        context_->CSSetShader(occlusion_shaders_[pass].Get(),nullptr,0); context_->CSSetShaderResources(0,7,inputs); context_->CSSetUnorderedAccessViews(0,4,outputs,nullptr); context_->Dispatch((w+7)/8,(h+7)/8,1);
        ID3D11ShaderResourceView* no_inputs[7]{}; ID3D11UnorderedAccessView* no_outputs[4]{}; context_->CSSetShaderResources(0,7,no_inputs); context_->CSSetUnorderedAccessViews(0,4,no_outputs,nullptr);
    };
    if(preset.slices) { dispatch(0,ao_width,ao_height); dispatch(1,ao_width,ao_height); }
    if(contact) dispatch(2,half_width,half_height);
    dispatch(3,width,height); context_->CSSetShader(nullptr,nullptr,0);
    const D3D11_BOX region{0,0,0,width,height,1};
    context_->CopySubresourceRegion(scene_color_.texture.Get(),0,0,0,0,ao_color_.texture.Get(),0,&region);
}
void D3D11Renderer::blur_scene(const gx::SceneBoundary& boundary) {
    const auto preset=blur_preset(controls_.effects.motion_blur); if(!preset.samples) return;
    const auto viewport=boundary.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
    const uint32_t width=uint32_t(std::clamp<long>(std::lround((viewport[0]+viewport[2])*efb_.width/640),1,efb_.width));
    const uint32_t height=uint32_t(std::clamp<long>(std::lround((viewport[1]+viewport[3])*efb_.height/480),1,efb_.height));
    const uint32_t render_width=uint32_t(std::clamp<long>(std::lround((viewport[0]+viewport[2])*scene_motion_.width/640),1,scene_motion_.width));
    const uint32_t render_height=uint32_t(std::clamp<long>(std::lround((viewport[1]+viewport[3])*scene_motion_.height/480),1,scene_motion_.height));
    const uint32_t tile_width=(render_width+15)/16,tile_height=(render_height+15)/16;
    if(blur_tiles_.width!=tile_width || blur_tiles_.height!=tile_height) blur_tiles_=image(tile_width,tile_height,1,DXGI_FORMAT_R16G16_FLOAT,nullptr,true);
    if(effect_color_.width!=efb_.width || effect_color_.height!=efb_.height) effect_color_=image(efb_.width,efb_.height,1,DXGI_FORMAT_R8G8B8A8_UNORM,nullptr,true);
    NativeBlurParams params{{width,height},{render_width,render_height},preset.shutter,preset.samples,16,0};
    upload(blur_constants_,&params,sizeof params,D3D11_BIND_CONSTANT_BUFFER); unbind_images(); context_->CSSetConstantBuffers(0,1,blur_constants_.buffer.GetAddressOf());
    for(size_t pass=0;pass<2;++pass) {
        ID3D11ShaderResourceView* inputs[]{efb_.srv.Get(),scene_motion_.srv.Get(),pass ? blur_tiles_.srv.Get() : nullptr};
        ID3D11UnorderedAccessView* outputs[]{pass ? effect_color_.uav.Get() : nullptr,pass ? nullptr : blur_tiles_.uav.Get()};
        context_->CSSetShader(blur_shaders_[pass].Get(),nullptr,0); context_->CSSetShaderResources(0,3,inputs); context_->CSSetUnorderedAccessViews(0,2,outputs,nullptr);
        context_->Dispatch(((pass ? width : tile_width)+7)/8,((pass ? height : tile_height)+7)/8,1);
        ID3D11ShaderResourceView* no_inputs[3]{}; ID3D11UnorderedAccessView* no_outputs[2]{}; context_->CSSetShaderResources(0,3,no_inputs); context_->CSSetUnorderedAccessViews(0,2,no_outputs,nullptr);
    }
    context_->CSSetShader(nullptr,nullptr,0); const D3D11_BOX region{0,0,0,width,height,1}; context_->CopySubresourceRegion(efb_.texture.Get(),0,0,0,0,effect_color_.texture.Get(),0,&region);
}

void D3D11Renderer::antialias_scene(const gx::SceneBoundary& boundary) {
    const auto viewport=boundary.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
    const uint32_t width=uint32_t(std::clamp<long>(std::lround((viewport[0]+viewport[2])*efb_.width/640),1,efb_.width));
    const uint32_t height=uint32_t(std::clamp<long>(std::lround((viewport[1]+viewport[3])*efb_.height/480),1,efb_.height));
    if(!smaa_area_.texture) {
        const D3D11_SUBRESOURCE_DATA area{areaTexBytes,AREATEX_PITCH,0},search{searchTexBytes,SEARCHTEX_PITCH,0};
        smaa_area_=image(AREATEX_WIDTH,AREATEX_HEIGHT,1,DXGI_FORMAT_R8G8_UNORM,&area);
        smaa_search_=image(SEARCHTEX_WIDTH,SEARCHTEX_HEIGHT,1,DXGI_FORMAT_R8_UNORM,&search);
    }
    if(smaa_source_.width!=width || smaa_source_.height!=height) {
        smaa_source_=image(width,height,1,DXGI_FORMAT_R8G8B8A8_UNORM,nullptr,true);
        smaa_edges_=image(width,height,1,DXGI_FORMAT_R8G8_UNORM,nullptr,true);
        smaa_weights_=image(width,height,1,DXGI_FORMAT_R8G8B8A8_UNORM,nullptr,true);
        smaa_output_=image(width,height,1,DXGI_FORMAT_R8G8B8A8_UNORM,nullptr,true);
    }
    unbind_images();
    const D3D11_BOX region{0,0,0,width,height,1};
    context_->CopySubresourceRegion(smaa_source_.texture.Get(),0,0,0,0,scene_color_.texture.Get(),0,&region);
    const std::array<float,4> metrics{1.f/width,1.f/height,float(width),float(height)};
    upload(smaa_constants_,metrics.data(),sizeof metrics,D3D11_BIND_CONSTANT_BUFFER);
    context_->CSSetConstantBuffers(0,1,smaa_constants_.buffer.GetAddressOf());
    ID3D11SamplerState* samplers[]{bloom_sampler_.Get(),sampler(0,false)}; context_->CSSetSamplers(0,2,samplers);
    const auto dispatch=[&](ID3D11ComputeShader* shader,const std::array<ID3D11ShaderResourceView*,5>& inputs,const std::array<ID3D11UnorderedAccessView*,3>& outputs) {
        context_->CSSetShader(shader,nullptr,0); context_->CSSetShaderResources(0,UINT(inputs.size()),inputs.data()); context_->CSSetUnorderedAccessViews(0,UINT(outputs.size()),outputs.data(),nullptr);
        context_->Dispatch((width+7)/8,(height+7)/8,1);
        const std::array<ID3D11ShaderResourceView*,5> no_inputs{}; const std::array<ID3D11UnorderedAccessView*,3> no_outputs{};
        context_->CSSetShaderResources(0,UINT(no_inputs.size()),no_inputs.data()); context_->CSSetUnorderedAccessViews(0,UINT(no_outputs.size()),no_outputs.data(),nullptr);
    };
    dispatch(smaa_shaders_[0].Get(),{smaa_source_.srv.Get(),nullptr,nullptr,nullptr,nullptr},{smaa_edges_.uav.Get(),nullptr,nullptr});
    dispatch(smaa_shaders_[1].Get(),{nullptr,smaa_edges_.srv.Get(),nullptr,smaa_area_.srv.Get(),smaa_search_.srv.Get()},{nullptr,smaa_weights_.uav.Get(),nullptr});
    dispatch(smaa_shaders_[2].Get(),{smaa_source_.srv.Get(),nullptr,smaa_weights_.srv.Get(),nullptr,nullptr},{nullptr,nullptr,smaa_output_.uav.Get()});
    if(sharpening_) {
        struct Constants { int32_t size[2]; float sharpness,unused; };
        static_assert(sizeof(Constants)==16);
        const Constants constants{{int32_t(width),int32_t(height)},rcas_constant(sharpness_),0};
        upload(rcas_constants_,&constants,sizeof constants,D3D11_BIND_CONSTANT_BUFFER); context_->CSSetConstantBuffers(0,1,rcas_constants_.buffer.GetAddressOf());
        dispatch(rcas_shader_.Get(),{smaa_output_.srv.Get(),nullptr,nullptr,nullptr,nullptr},{smaa_source_.uav.Get(),nullptr,nullptr});
    }
    context_->CSSetShader(nullptr,nullptr,0);
    context_->CopySubresourceRegion(efb_.texture.Get(),0,0,0,0,sharpening_ ? smaa_source_.texture.Get() : smaa_output_.texture.Get(),0,&region);
}

void D3D11Renderer::bloom_scene(const gx::SceneBoundary& boundary) {
    const auto viewport=boundary.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
    const BloomLevel region{
        uint32_t(std::clamp<long>(std::lround((viewport[0]+viewport[2])*efb_.width/640),1,efb_.width)),
        uint32_t(std::clamp<long>(std::lround((viewport[1]+viewport[3])*efb_.height/480),1,efb_.height))};
    if(region.width!=bloom_region_.width || region.height!=bloom_region_.height) {
        for(uint32_t i=0;i<6;++i) {
            const auto extent=bloom_level_extent(region.width,region.height,i);
            bloom_down_[i]=image(extent.width,extent.height,1,DXGI_FORMAT_R16G16B16A16_FLOAT,nullptr,true);
            if(i<5) bloom_up_[i]=image(extent.width,extent.height,1,DXGI_FORMAT_R16G16B16A16_FLOAT,nullptr,true);
        }
        bloom_region_=region;
    }
    if(!hdr_active_ && (bloom_color_.width!=efb_.width || bloom_color_.height!=efb_.height)) bloom_color_=image(efb_.width,efb_.height,1,DXGI_FORMAT_R8G8B8A8_UNORM,nullptr,true);
    struct Params { uint32_t src[2],dst[2],region[2]; float weight,weight_a,scale,headroom; uint32_t reserved[2]; };
    static_assert(sizeof(Params)==48);
    Params params{}; params.region[0]=region.width; params.region[1]=region.height; params.scale=params.weight_a=1; params.headroom=hdr_active_ ? std::max(1.f,float(hdr_.peak_nits)/hdr_.paper_white_nits) : kBloomSdrHeadroom;
    unbind_images();
    const auto dispatch=[&](size_t shader,const Image& a,const Image* b,Image& output,bool composite=false) {
        params.src[0]=a.width; params.src[1]=a.height; params.dst[0]=output.width; params.dst[1]=output.height;
        upload(bloom_constants_,&params,sizeof params,D3D11_BIND_CONSTANT_BUFFER);
        context_->CSSetShader(bloom_shaders_[shader].Get(),nullptr,0); context_->CSSetConstantBuffers(0,1,bloom_constants_.buffer.GetAddressOf()); context_->CSSetSamplers(0,1,bloom_sampler_.GetAddressOf());
        ID3D11ShaderResourceView* inputs[]{a.srv.Get(),b ? b->srv.Get() : nullptr}; context_->CSSetShaderResources(0,2,inputs);
        ID3D11UnorderedAccessView* outputs[]{composite ? nullptr : output.uav.Get(),composite ? output.uav.Get() : nullptr}; context_->CSSetUnorderedAccessViews(0,2,outputs,nullptr);
        const uint32_t width=composite ? region.width : output.width,height=composite ? region.height : output.height; context_->Dispatch((width+7)/8,(height+7)/8,1);
        ID3D11ShaderResourceView* no_inputs[2]{}; ID3D11UnorderedAccessView* no_outputs[2]{};
        context_->CSSetShaderResources(0,2,no_inputs); context_->CSSetUnorderedAccessViews(0,2,no_outputs,nullptr);
    };
    dispatch(0,efb_,nullptr,bloom_down_[0]);
    for(size_t i=1;i<6;++i) dispatch(1,bloom_down_[i-1],nullptr,bloom_down_[i]);
    for(int i=4;i>=0;--i) {
        params.weight=kBloomWeights[size_t(i)]; params.weight_a=i==4 ? kBloomWeights[5] : 1.f; params.scale=i==0 ? bloom_mix(bloom_,hdr_active_) : 1.f;
        dispatch(2,i==4 ? bloom_down_[5] : bloom_up_[size_t(i)+1],&bloom_down_[size_t(i)],bloom_up_[size_t(i)]);
    }
    if(!hdr_active_) dispatch(3,bloom_up_[0],&efb_,bloom_color_,true);
    context_->CSSetShader(nullptr,nullptr,0);
    if(hdr_active_) bloom_valid_=true;
    else { const D3D11_BOX box{0,0,0,region.width,region.height,1}; context_->CopySubresourceRegion(efb_.texture.Get(),0,0,0,0,bloom_color_.texture.Get(),0,&box); }
}

void D3D11Renderer::set_menu(std::function<void()> build,std::function<void()> setup) {
    owner();
    if(overlay_ && setup && !menu_setup_) shutdown_overlay();
    menu_build_=std::move(build); if(setup) menu_setup_=std::move(setup);
}
void D3D11Renderer::initialize_overlay() {
    if(overlay_) return;
    auto* previous=ImGui::GetCurrentContext(); overlay_=ImGui::CreateContext(); ImGui::SetCurrentContext(previous);
    try {
        ImGuiScope current(overlay_); auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.LogFilename=nullptr; io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
        if(menu_setup_) menu_setup_(); else load_prompt_font(font_bytes_);
        if(!ImGui_ImplSDL3_InitForD3D(window_)) throw std::runtime_error("D3D11 overlay SDL initialization failed");
        if(!ImGui_ImplDX11_Init(device_.Get(),context_.Get())) throw std::runtime_error("D3D11 ImGui initialization failed");
    } catch(...) { shutdown_overlay(); throw; }
}
bool D3D11Renderer::menu_event(const SDL_Event& event) {
    owner(); if(!overlay_) return false;
    ImGuiScope current(overlay_); ImGui_ImplSDL3_ProcessEvent(&event); const auto& io=ImGui::GetIO(); return io.WantCaptureMouse || io.WantCaptureKeyboard || io.WantTextInput;
}
void D3D11Renderer::draw_overlay(const gx::FramePacket& packet,Image& target) {
    initialize_overlay(); ImGuiScope current(overlay_);
    ImGui_ImplDX11_NewFrame(); ImGui_ImplSDL3_NewFrame(); auto& io=ImGui::GetIO(); io.DisplaySize={float(target.width),float(target.height)}; io.DisplayFramebufferScale={1,1}; ImGui::NewFrame();
    if(!packet.black_output) for(const auto& prompt:packet.prompts) draw_prompt_picture(prompt,{target.width,target.height},packet.display_source,prompt_style_);
    if(menu_build_) menu_build_(); ImGui::Render();
    unbind_images(); context_->OMSetRenderTargets(1,target.rtv.GetAddressOf(),nullptr); ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); unbind_images();
}
void D3D11Renderer::shutdown_overlay() {
    if(!overlay_) return;
    auto* previous=ImGui::GetCurrentContext(); ImGui::SetCurrentContext(overlay_);
    if(ImGui::GetIO().BackendRendererUserData) ImGui_ImplDX11_Shutdown();
    if(ImGui::GetIO().BackendPlatformUserData) ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(overlay_); ImGui::SetCurrentContext(previous==overlay_ ? nullptr : previous); overlay_=nullptr; font_bytes_.clear();
}

} // namespace
std::unique_ptr<NativeRenderer> make_d3d11_renderer() { return std::make_unique<D3D11Renderer>(); }
} // namespace uc
