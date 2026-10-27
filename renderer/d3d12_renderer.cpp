#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "native_renderer.h"
#include "native_tev.h"
#include "late_aim.h"
#include "render_scale.h"
#include "native_viewport.h"
#include "bloom_math.h"
#include "hdr_math.h"
#include "upscale_mode.h"
#include "upscale_math.h"
#include "AreaTex.h"
#include "SearchTex.h"
#include "motion_tracker.h"
#include "native_effects.h"
#include "scene_lights.h"
#include "view_reconstruct.h"
#include "shadow_math.h"
#include "native_upscaler.h"
#include "streamline.h"
#include "native_frame_generation.h"
#include "native_rt_effects.h"
#include "native_dlss5.h"
#include "rt_reflect_math.h"
#include "rt_shadow_math.h"
#include "scene_boundary.h"
#include "post_effects.h"
#include "prompt_picture.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_dx12.h"
#include <intrin.h>
#include <SDL3/SDL.h>
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace uc {
namespace {
using Microsoft::WRL::ComPtr;
using Clock=std::chrono::steady_clock;
constexpr DXGI_FORMAT color_format=DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr size_t slot_count=16;   // submissions in flight, like the Vulkan backend: a frame is several packets and the guest must run ahead of the GPU
constexpr UINT texture_registers=11;
void check(HRESULT result,const char* operation) {
    if(FAILED(result)) throw std::runtime_error(std::string(operation)+" (HRESULT "+std::to_string(uint32_t(result))+")");
}
ComPtr<IDXGIFactory2> presentation_factory(IDXGIFactory2* factory,Streamline* streamline) {
    ComPtr<IDXGIFactory2> owned=factory;
    if(streamline) {
        // SR/RR also require the Present hook to retire tags and SDK resources.
        void* upgraded=owned.Detach(); std::string reason;
        const bool ok=streamline->upgrade_native_interface(&upgraded,reason);
        owned.Attach(static_cast<IDXGIFactory2*>(upgraded));
        if(!ok) throw std::runtime_error(reason);
    }
    return owned;
}
size_t aligned(size_t size,size_t alignment=256) { return (size+alignment-1)&~(alignment-1); }
std::array<float,4> copy_uv(const std::array<uint32_t,4>& region,uint32_t width,uint32_t height) {
    const uint32_t left=uint32_t(uint64_t(region[0])*width/640),top=uint32_t(uint64_t(region[1])*height/480);
    const uint32_t right=std::max(left+1,uint32_t(uint64_t(region[0]+region[2])*width/640)),bottom=std::max(top+1,uint32_t(uint64_t(region[1]+region[3])*height/480));
    return {left/float(width),top/float(height),(right-left)/float(width),(bottom-top)/float(height)};
}
std::vector<char> shader_bytes(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input) throw std::runtime_error("Cannot open DirectX shader: "+path.string());
    const auto size=input.tellg();
    if(size<=0) throw std::runtime_error("Empty DirectX shader: "+path.string());
    std::vector<char> bytes(size_t(size),char{});
    input.seekg(0); input.read(bytes.data(),size);
    if(!input) throw std::runtime_error("Cannot read DirectX shader: "+path.string());
    return bytes;
}
struct DrawPush {
    gx::Projection projection;
    uint32_t palette=0,current_row=0,indexed=0,alpha_pass=0,bank=UINT32_MAX,motion=UINT32_MAX;
    float jitter[2]{},viewport[4]{};
    uint32_t effects=0,record=UINT32_MAX;
    float shift[2]{};
};
static_assert(sizeof(DrawPush)==128 && offsetof(DrawPush,shift)==120);
struct MotionBlock { gx::Projection previous; uint32_t palette=0,kind=0,unused[2]{}; };
static_assert(sizeof(MotionBlock)==80);
struct BloomParams {
    uint32_t src[2]{},dst[2]{},region[2]{};
    float weight=0,weight_a=1,scale=1,headroom=kBloomSdrHeadroom;
    uint32_t reserved[2]{};
};
static_assert(sizeof(BloomParams)==48);
struct Image {
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12DescriptorHeap> views;
    ComPtr<ID3D12DescriptorHeap> sampled_view;
    ComPtr<ID3D12DescriptorHeap> logic_view;
    uint32_t width=0,height=0,mips=1;
    DXGI_FORMAT format=color_format;
    gx::TextureChannels channels=gx::TextureChannels::rgba;
    std::vector<D3D12_RESOURCE_STATES> states;
    std::weak_ptr<const gx::TexturePixels> source;
    uint64_t written=0,allocation_size=0;
    uint32_t guest_address=0;
};
struct Mesh {
    uint64_t id=0;
    uint64_t rows=0;
    std::weak_ptr<const gx::MeshBuffers> source;
    ComPtr<ID3D12Resource> buffer;
    D3D12_VERTEX_BUFFER_VIEW vertices{};
    D3D12_INDEX_BUFFER_VIEW indices{};
};
using TextureTableKey=std::array<uintptr_t,11>;
struct TextureTableHash {
    size_t operator()(const TextureTableKey& key) const {
        size_t hash=0; for(const auto value:key) hash^=value+0x9e3779b9u+(hash<<6)+(hash>>2); return hash;
    }
};
struct StructuredView { size_t offset=0; UINT descriptor=0; };
struct Slot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12DescriptorHeap> source_descriptors;
    std::unordered_map<TextureTableKey,UINT,TextureTableHash> texture_tables;
    std::unordered_map<const gx::Transforms*,StructuredView> palettes;
    std::unordered_map<const gx::TextureBank*,StructuredView> banks;
    std::vector<ComPtr<ID3D12DescriptorHeap>> sampler_heaps;
    std::map<std::array<uint16_t,8>,UINT> sampler_tables;
    ComPtr<ID3D12Resource> upload,timing;
    std::byte* mapped=nullptr;
    size_t capacity=0,used=0;
    UINT descriptor_capacity=0,descriptor_used=0;
    UINT source_used=0;
    UINT sampler_used=0;
    uint64_t fence=0,serial=0;
    std::vector<std::shared_ptr<Image>> images;
    std::vector<ComPtr<ID3D12Resource>> resources;
    std::vector<std::shared_ptr<NativeRtScene::Frame>> rt_frames;
    std::vector<std::shared_ptr<NativeRtWork>> rt_work;
    std::vector<std::shared_ptr<NativeDlss5Pass::Work>> dlss5_work;
};
class D3D12Renderer final : public NativeRenderer {
public:
    ~D3D12Renderer() override { try { shutdown(); } catch(const std::exception& error) { std::fprintf(stderr,"DirectX 12 shutdown: %s\n",error.what()); } }
    void initialize(SDL_Window*,bool,const std::filesystem::path&,double,double,bool) override;
    bool render(const gx::FramePacket&,bool,uint64_t) override;
    void resize(uint32_t,uint32_t) override;
    void set_render_scale(double) override;
    void set_controls(const NativeControls&) override;
    void set_bloom(const BloomSettings& settings) override;
    void set_hdr(const HdrSettings&) override;
    void hdr_state_changed() override { output_dirty_=true; invalidate_held(); }
    bool hdr_active() const override { return hdr_active_; }
    bool set_upscaling(UpscalerKind,UpscaleMode,bool,double,bool,uint32_t) override;
    UpscalerKind active_upscaler() const override { return upscaler_kind_; }
    bool frame_generation_active() const override { return generation_requested_ && generation_ && generation_kind_==upscaler_kind_; }
    RtScene::Stats rt_stats() const override { return rt_scene_.stats(); }
    void allow_dlss5(bool allowed) override { dlss5_allowed_=allowed; }
    void set_dlss5(const Dlss5Settings&) override;
    bool dlss5_active() const override { return dlss5_pass_.ran(); }
    std::string dlss5_status() const override { return dlss5_gap_.empty() ? dlss5_pass_.status() : dlss5_gap_; }
    RenderPixels capture_scene() override;
    MotionPixels capture_motion() override;
    void set_menu(std::function<void()> build,std::function<void()> setup) override;
    bool menu_event(const SDL_Event&) override;
    void set_prompt_style(PromptStyle style) override { prompt_style_=style; }
    void set_reticle_layer(bool) override;
    bool reticle_layer_ready() const override;
    bool held_presented() const override { return held_presented_; }
    bool held_has_reticle() const override { return !held_draws_.empty(); }
    bool render_overlay(uint64_t) override;
    bool copy_ready(const std::shared_ptr<const gx::TexturePixels>&,bool) override;
    std::vector<const gx::TexturePixels*> copy_keys() const override;
    void lost_copies(std::vector<const gx::TexturePixels*> keys) override { lost_.insert(keys.begin(),keys.end()); }
    std::vector<uint64_t> poll_completions() override;
    bool wait_for_pending(uint64_t) override;
    RenderPixels capture(uint64_t,bool) override;
    NativeStats stats() const override;
    void shutdown() override;
private:
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapchain_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12InfoQueue> info_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12DescriptorHeap> samplers_;
    ComPtr<ID3D12QueryHeap> queries_;
    std::array<ComPtr<ID3D12Resource>,2> backbuffers_;
    ComPtr<ID3D12DescriptorHeap> backbuffer_views_;
    std::array<Slot,slot_count> slots_;
    std::shared_ptr<Image> efb_,depth_,output_,white_,black_;
    std::unordered_map<const gx::TexturePixels*,std::shared_ptr<Image>> textures_,copies_;
    std::vector<std::shared_ptr<Image>> recycled_copies_;
    uint64_t recycled_copy_bytes_=0;
    struct Alias { std::weak_ptr<const gx::TexturePixels> requested; std::shared_ptr<Image> image; };
    std::unordered_map<const gx::TexturePixels*,Alias> aliases_;
    std::unordered_map<uint32_t,std::weak_ptr<Image>> latest_copies_;
    std::unordered_map<const gx::MeshBuffers*,Mesh> meshes_;
    std::set<const gx::TexturePixels*> lost_;
    std::map<uint64_t,ComPtr<ID3D12PipelineState>> pipelines_;
    // TEV-specialized pixel shaders: a PSO per (state key, shader), built on the compile threads; the ubershader PSO draws until it lands. DXBC throughout (a PSO cannot mix it with DXIL).
    using SpecKey=std::pair<uint64_t,const void*>;
    struct SpecDone { SpecKey key; ComPtr<ID3D12PipelineState> state; };
    std::unique_ptr<TevShaders> tev_;
    std::map<SpecKey,ComPtr<ID3D12PipelineState>> spec_pipelines_;   // a null state: creation failed, the ubershader stays
    std::set<SpecKey> spec_pending_;
    std::mutex spec_mutex_;
    std::vector<SpecDone> spec_done_;
    std::vector<char> dxbc_vertex_,dxbc_motion_vertex_;
    ComPtr<ID3D12PipelineState> blit_pipeline_;
    ComPtr<ID3D12RootSignature> bloom_root_;
    std::array<ComPtr<ID3D12PipelineState>,4> bloom_pipelines_;
    std::array<std::shared_ptr<Image>,6> bloom_down_;
    std::array<std::shared_ptr<Image>,5> bloom_up_;
    std::shared_ptr<Image> bloom_color_;
    BloomLevel bloom_region_{};
    bool bloom_valid_=false;
    HdrSettings hdr_;
    bool hdr_active_=false,output_dirty_=true,hdr_metadata_dirty_=true;
    ComPtr<ID3D12PipelineState> hdr_pipeline_;
    std::shared_ptr<Image> ui_image_;
    UpscalerKind upscaler_kind_=UpscalerKind::off;
    UpscaleMode upscale_mode_=UpscaleMode::native;
    std::unique_ptr<Streamline> streamline_;
    std::unique_ptr<NativeUpscaler> upscaler_;
    std::unique_ptr<NativeFrameGeneration> generation_;
    bool generation_requested_=false,generation_recreate_=false,frame_scene_=false,hudless_taken_=false,generation_prepared_=false;
    uint32_t generation_multiplier_=2;
    UpscalerKind generation_kind_=UpscalerKind::off;
    uint64_t retired_generated_=0;
    std::array<std::shared_ptr<Image>,2> hudless_;
    std::shared_ptr<Image> hudless_sdr_;
    std::shared_ptr<Image> raster_color_,raster_depth_,ao_output_;
    RenderExtent internal_{},last_sdk_render_{},last_sdk_output_{};
    std::array<float,2> jitter_{};
    uint64_t upscale_frame_=0,guest_frame_=0;
    Clock::time_point last_upscale_{};
    gx::Projection scene_projection_{};
    bool raster_initialized_=false;
    bool upscaler_dirty_=false,recording_=false;
    std::optional<double> pending_scale_;
    std::optional<NativeControls> pending_controls_;
    struct SceneClear { bool pending=false; std::array<uint32_t,4> source{}; std::array<uint8_t,4> color{}; uint32_t depth=0xFFFFFF; } scene_clear_;
    ComPtr<ID3D12PipelineState> depth_blit_pipeline_;
    bool sharpening_=false;
    double sharpness_=0.5;
    ComPtr<ID3D12RootSignature> smaa_root_;
    std::array<ComPtr<ID3D12PipelineState>,4> smaa_pipelines_;
    std::shared_ptr<Image> smaa_source_,smaa_edges_,smaa_weights_,smaa_output_,smaa_area_,smaa_search_;
    std::shared_ptr<Image> scene_color_,scene_depth_,scene_motion_,scene_gbuffer_;
    std::unique_ptr<gx::MotionTracker> tracker_;
    bool scene_captured_=false,motion_cleared_=false;
    std::vector<char> motion_vertex_,motion_pixel_,logic_pixel_,motion_logic_pixel_;
    gx::SceneLightCollector scene_lights_;
    std::optional<gx::CollectedLight> dominant_light_;
    std::optional<gx::Projection> frame_projection_;
    bool reset_next_scene_=true;
    ComPtr<ID3D12RootSignature> effects_root_;
    std::array<ComPtr<ID3D12PipelineState>,6> effects_pipelines_;
    std::shared_ptr<Image> ao_raw_,ao_filtered_,contact_,effect_output_,effect_source_,blur_tiles_;
    std::shared_ptr<Image> shadow_map_;
    std::optional<ShadowFit> shadow_fit_;
    ComPtr<ID3D12PipelineState> shadow_pipeline_;
    bool shadow_cleared_=false,shadow_valid_=false;
    std::filesystem::path shaders_;
    BloomSettings bloom_{false,100};
    gx::SceneBoundary boundary_;
    std::optional<gx::TextureCopy> scene_restore_copy_;
    bool frame_start_=true,scene_phase_=false;
    bool reticle_layer_=false,held_valid_=false,held_presented_=false;
    std::shared_ptr<Image> held_color_,held_depth_;
    std::vector<gx::DrawPacket> frame_reticle_,held_draws_;
    std::vector<gx::PromptDraw> held_prompts_;
    std::array<uint32_t,4> held_source_{0,0,640,480};
    std::array<float,2> held_aim_{};
    std::vector<char> vertex_shader_,pixel_shader_,blit_vertex_,blit_pixel_;
    NativeControls controls_;
    GraphicsEffects effects_;
    NativeRtScene rt_scene_;
    std::unique_ptr<NativeRtEffects> rt_effects_;
    std::future<std::unique_ptr<NativeRtEffects>> rt_effects_warm_;   // its eleven DXIL pipelines compile at startup, so turning path tracing on later does not stall on a cold driver cache
    std::shared_ptr<NativeRtWork> rt_work_;
    std::shared_ptr<Image> scene_material_,scene_albedo_;
    std::vector<NativeRtGeometry> rt_geometry_;
    std::vector<RtMaterialRecord> rt_records_;
    std::vector<gx::LightUniform> rt_lights_;
    std::array<float,3> rt_overhead_=kRtOverheadDirection;
    bool rt_supported_=false,rr_supported_=false;
    uint64_t next_mesh_id_=1;
    NativeDlss5Pass dlss5_pass_;
    NativeDlss5Context dlss5_context_;
    Dlss5Settings dlss5_settings_;
    std::optional<Dlss5Settings> pending_dlss5_;
    bool dlss5_allowed_=true,dlss5_device_=false,dlss5_scene_ready_=false,dlss5_scene_reset_=true;
    RenderExtent dlss5_scene_size_{};
    std::string dlss5_gap_;
    SDL_Window* window_=nullptr;
    ImGuiContext* ui_=nullptr;
    std::function<void()> menu_build_,menu_setup_;
    PromptStyle prompt_style_=PromptStyle::monochrome;
    std::vector<char> font_bytes_;
    ComPtr<ID3D12DescriptorHeap> ui_heap_;
    std::array<bool,128> ui_allocated_{};
    std::array<uint64_t,128> ui_retired_{};
    NativeStats stats_;
    std::vector<uint64_t> completed_;
    HANDLE event_=nullptr;
    uint64_t next_fence_=1,last_serial_=0,last_efb_=0,last_output_=0,timestamp_frequency_=0,swept_fence_=0;
    UINT srv_stride_=0,rtv_stride_=0,sampler_stride_=0;
    uint32_t width_=0,height_=0;
    double scale_=0.5,aspect_=16.0/9;
    bool initialized_=false,efb_initialized_=false,tearing_=false,exclusive_=false;
    ComPtr<ID3D12Resource> buffer(size_t,D3D12_HEAP_TYPE=D3D12_HEAP_TYPE_UPLOAD);
    ComPtr<ID3D12DescriptorHeap> heap(D3D12_DESCRIPTOR_HEAP_TYPE,UINT,bool=false);
    std::shared_ptr<Image> image(uint32_t,uint32_t,uint32_t,DXGI_FORMAT,bool,bool=false,bool=false);
    void transition(Slot&,Image&,D3D12_RESOURCE_STATES,UINT=UINT32_MAX);
    void collect();
    bool wait(uint64_t,uint64_t);
    void idle();
    Slot* begin(size_t,UINT);
    uint64_t submit(Slot&,uint64_t);
    size_t upload(Slot&,const void*,size_t,size_t=256);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(Slot&,UINT) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(Slot&,UINT) const;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(const Image&,UINT=0) const;
    D3D12_CPU_DESCRIPTOR_HANDLE logic_rtv(Image&);
    UINT descriptors(Slot&);
    void srv(Slot&,UINT,const Image&,UINT=0,UINT=UINT32_MAX);
    void structured(Slot&,UINT,size_t,UINT);
    D3D12_CPU_DESCRIPTOR_HANDLE source_cpu(Slot&,UINT) const;
    D3D12_CPU_DESCRIPTOR_HANDLE sampled_view(Image&);
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_table(Slot&,const std::array<uint16_t,8>&);
    std::shared_ptr<Image> texture(Slot&,const std::shared_ptr<const gx::TexturePixels>&);
    std::shared_ptr<Image> upload_texture(Slot&,const gx::TexturePixels*);
    std::shared_ptr<Image> copy_texture(Slot&,const gx::TextureCopy&);
    void forget_copy_address(const Image&);
    void recycle_copy(std::shared_ptr<Image>);
    void samplers();
    void targets();
    ID3D12PipelineState* pipeline(const gx::DrawPacket&,bool,bool,bool=false,const TevWords* tev=nullptr);
    // What the command list currently has bound, so draws skip state they would only set again (the driver replays every call at ExecuteCommandLists). Reset wherever other recording code runs between draws.
    struct Bound {
        ID3D12PipelineState* pso=nullptr; D3D_PRIMITIVE_TOPOLOGY topology=D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE,5> colors{}; UINT color_count=0; SIZE_T depth=0;
        D3D12_VIEWPORT viewport{}; D3D12_RECT scissor{}; bool rasterizer_set=false;
        D3D12_GPU_DESCRIPTOR_HANDLE samplers{}, textures{}; D3D12_VERTEX_BUFFER_VIEW vertex{}; D3D12_INDEX_BUFFER_VIEW index{};
    } bound_;
    void bind_pso(Slot& slot,ID3D12PipelineState* state) { if(bound_.pso!=state) { slot.command->SetPipelineState(state); bound_.pso=state; } }
    void bind_targets(Slot& slot,UINT count,const std::array<D3D12_CPU_DESCRIPTOR_HANDLE,5>& colors,D3D12_CPU_DESCRIPTOR_HANDLE depth) {
        bool same=bound_.color_count==count && bound_.depth==depth.ptr;
        for(UINT i=0;same && i<count;++i) same=bound_.colors[i].ptr==colors[i].ptr;
        if(same) return;
        slot.command->OMSetRenderTargets(count,colors.data(),FALSE,&depth); bound_.color_count=count; bound_.colors=colors; bound_.depth=depth.ptr;
    }
    void drain_spec() {   // the specialized PSOs the compile threads finished become usable (render thread)
        std::lock_guard lock(spec_mutex_);
        for(auto& done:spec_done_) { spec_pipelines_[done.key]=std::move(done.state); ++stats_.state_objects; }
        spec_done_.clear();
    }
    void blit(Slot&,Image&,UINT,Image&,UINT,const std::array<float,4>&,uint32_t=0,bool=true,RenderExtent={});
    void draw(Slot&,const gx::DrawPacket&,const gx::FramePacket&,Image&,Image&,bool=false,bool=false);
    void clear(Slot&,const gx::FramePacket&,const D3D12_RECT*);
    void overlay(Slot&,const gx::FramePacket&,Image&);
    void destroy_overlay();
    void initialize_bloom();
    void record_bloom(Slot&,bool);
    void bloom_dispatch(Slot&,UINT,const std::shared_ptr<Image>&,const std::shared_ptr<Image>&,const std::shared_ptr<Image>&,const BloomParams&);
    void invalidate_held();
    void rebuild_output();
    void hdr_metadata();
    void antialias_scene(Slot&,bool=false);
    void scene_targets(Slot&);
    void snapshot_scene(Slot&);
    void reset_scene();
    std::vector<std::byte> readback(Image&);
    bool wants_scene() const { return upscaler_kind_!=UpscalerKind::off || controls_.scene_targets || needs_scene_targets(effects_) || dlss5_requested(); }
    bool dlss5_requested() const { return dlss5_device_ && dlss5_settings_.enabled() && !dlss5_pass_.failed(); }
    bool record_dlss5(Slot&,Dlss5Hook,const std::shared_ptr<Image>&,std::array<int32_t,2>,RenderExtent,std::array<float,2>,bool);
    bool rt_active() const { return any_rt_active(effects_,rt_supported_); }
    bool rr_active() const { return rt_active() && effects_.ray_reconstruction && upscaler_kind_==UpscalerKind::dlss && rr_supported_; }
    void record_rt_geometry(Slot&);
    bool ray_trace(Slot&,bool);
    bool reconstruction() const { return uses_jitter(upscaler_kind_); }
    void configure_upscaler();
    void reconstruct_scene(Slot&,bool);
    void copy_depth(Slot&,RenderExtent,RenderExtent);
    void clear_raster(Slot&,const gx::FramePacket&,const std::array<uint32_t,4>*);
    void initialize_effects();
    void effect_dispatch(Slot&,UINT,const void*,size_t,const std::array<std::shared_ptr<Image>,7>&,const std::array<std::shared_ptr<Image>,4>&,uint32_t,uint32_t);
    void occlusion(Slot&);
    void motion_blur(Slot&);
    void shadow_draw(Slot&,const gx::DrawPacket&,const D3D12_VERTEX_BUFFER_VIEW&,const D3D12_INDEX_BUFFER_VIEW&,UINT);
    void encode_hdr(Slot&,const gx::FramePacket&,UINT,Image*,Image* destination=nullptr,Image* source=nullptr);
    void take_hudless(Slot&,const gx::FramePacket&);
    void configure_generation();
};

ComPtr<ID3D12DescriptorHeap> D3D12Renderer::heap(D3D12_DESCRIPTOR_HEAP_TYPE type,UINT count,bool visible) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{}; desc.Type=type; desc.NumDescriptors=count;
    desc.Flags=visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> result; check(device_->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&result)),"CreateDescriptorHeap");
    if(visible) ++stats_.descriptor_pools;
    return result;
}
ComPtr<ID3D12Resource> D3D12Renderer::buffer(size_t size,D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{}; properties.Type=type;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=std::max<size_t>(256,size);
    desc.Height=1; desc.DepthOrArraySize=1; desc.MipLevels=1; desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    check(device_->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,type==D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&result)),"Create buffer");
    return result;
}
std::shared_ptr<Image> D3D12Renderer::image(uint32_t width,uint32_t height,uint32_t mips,DXGI_FORMAT format,bool target,bool depth,bool unordered) {
    if(!width || !height || width>16384 || height>16384 || !mips || mips>std::bit_width(std::max(width,height))) throw std::invalid_argument("Invalid DirectX image dimensions");
    auto result=std::make_shared<Image>(); result->width=width; result->height=height; result->mips=mips; result->format=format;
    const auto initial=depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : target ? D3D12_RESOURCE_STATE_RENDER_TARGET : unordered ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST;
    result->states.assign(mips,initial);
    D3D12_HEAP_PROPERTIES properties{}; properties.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=width; desc.Height=height;
    desc.DepthOrArraySize=1; desc.MipLevels=UINT16(mips); desc.Format=depth ? DXGI_FORMAT_R32_TYPELESS : target && format==color_format ? DXGI_FORMAT_R8G8B8A8_TYPELESS : format; desc.SampleDesc.Count=1;
    desc.Flags=depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
    if(unordered) desc.Flags|=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_CLEAR_VALUE clear{}; clear.Format=format; if(depth) clear.DepthStencil.Depth=1.f;
    check(device_->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,initial,target||depth ? &clear : nullptr,IID_PPV_ARGS(&result->resource)),"Create texture");
    result->allocation_size=device_->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
    if(depth) {
        result->format=DXGI_FORMAT_R32_FLOAT;
        result->views=heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV,1);
        D3D12_DEPTH_STENCIL_VIEW_DESC view{}; view.Format=DXGI_FORMAT_D32_FLOAT; view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(result->resource.Get(),&view,result->views->GetCPUDescriptorHandleForHeapStart());
    } else if(target) {
        result->views=heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV,mips);
        for(UINT mip=0;mip<mips;++mip) {
            D3D12_RENDER_TARGET_VIEW_DESC view{}; view.Format=format; view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D; view.Texture2D.MipSlice=mip;
            device_->CreateRenderTargetView(result->resource.Get(),&view,rtv(*result,mip));
        }
    }
    ++stats_.textures; return result;
}
void D3D12Renderer::transition(Slot& slot,Image& image,D3D12_RESOURCE_STATES after,UINT mip) {
    for(UINT i=0;i<image.mips;++i) if((mip==UINT32_MAX || mip==i) && image.states[i]!=after) {
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={image.resource.Get(),i,image.states[i],after};
        slot.command->ResourceBarrier(1,&barrier); image.states[i]=after;
    }
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::rtv(const Image& image,UINT mip) const {
    auto result=image.views->GetCPUDescriptorHandleForHeapStart(); result.ptr+=size_t(mip)*rtv_stride_; return result;
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::logic_rtv(Image& image) {
    if(!image.logic_view) {
        image.logic_view=heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1);
        D3D12_RENDER_TARGET_VIEW_DESC view{}; view.Format=DXGI_FORMAT_R8G8B8A8_UINT; view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
        device_->CreateRenderTargetView(image.resource.Get(),&view,image.logic_view->GetCPUDescriptorHandleForHeapStart());
    }
    return image.logic_view->GetCPUDescriptorHandleForHeapStart();
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::cpu(Slot& slot,UINT index) const {
    auto result=slot.descriptors->GetCPUDescriptorHandleForHeapStart(); result.ptr+=size_t(index)*srv_stride_; return result;
}
D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::gpu(Slot& slot,UINT index) const {
    auto result=slot.descriptors->GetGPUDescriptorHandleForHeapStart(); result.ptr+=uint64_t(index)*srv_stride_; return result;
}
UINT D3D12Renderer::descriptors(Slot& slot) {
    if(slot.descriptor_used+texture_registers>slot.descriptor_capacity) throw std::runtime_error("DirectX descriptor reservation exhausted");
    const UINT index=slot.descriptor_used; slot.descriptor_used+=texture_registers; return index;
}
size_t D3D12Renderer::upload(Slot& slot,const void* source,size_t bytes,size_t alignment) {
    const auto offset=aligned(slot.used,alignment);
    if(offset>slot.capacity || bytes>slot.capacity-offset) throw std::runtime_error("DirectX upload reservation exhausted");
    if(source) std::memcpy(slot.mapped+offset,source,bytes);
    slot.used=offset+bytes; return offset;
}
void D3D12Renderer::srv(Slot& slot,UINT index,const Image& image,UINT first,UINT count) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{}; desc.Format=image.format; desc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping=image.channels==gx::TextureChannels::alpha_intensity ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(3,3,3,3) : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MostDetailedMip=first; desc.Texture2D.MipLevels=count==UINT32_MAX ? image.mips-first : count;
    device_->CreateShaderResourceView(image.resource.Get(),&desc,cpu(slot,index));
}
void D3D12Renderer::structured(Slot& slot,UINT index,size_t offset,UINT count) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{}; desc.ViewDimension=D3D12_SRV_DIMENSION_BUFFER; desc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.FirstElement=offset/16; desc.Buffer.NumElements=count; desc.Buffer.StructureByteStride=16;
    device_->CreateShaderResourceView(slot.upload.Get(),&desc,source_cpu(slot,index));
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::source_cpu(Slot& slot,UINT index) const {
    if(index>=slot.descriptor_capacity) throw std::runtime_error("DirectX source descriptor reservation exhausted");
    auto handle=slot.source_descriptors->GetCPUDescriptorHandleForHeapStart(); handle.ptr+=size_t(index)*srv_stride_; return handle;
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::sampled_view(Image& image) {
    if(!image.sampled_view) {
        image.sampled_view=heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1);
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{}; desc.Format=image.format; desc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping=image.channels==gx::TextureChannels::alpha_intensity ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(3,3,3,3) : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels=image.mips;
        device_->CreateShaderResourceView(image.resource.Get(),&desc,image.sampled_view->GetCPUDescriptorHandleForHeapStart());
    }
    return image.sampled_view->GetCPUDescriptorHandleForHeapStart();
}
D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::sampler_table(Slot& slot,const std::array<uint16_t,8>& modes) {
    auto found=slot.sampler_tables.find(modes);
    if(found==slot.sampler_tables.end()) {
        if(slot.sampler_used==2048) {
            // A new immutable page keeps earlier draws' sampler tables alive until this slot retires.
            if(slot.sampler_heaps.size()>=16) throw std::invalid_argument("DirectX packet exceeds sampler table capacity");
            slot.sampler_heaps.push_back(heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,2048,true)); slot.sampler_tables.clear(); slot.sampler_used=0;
            ID3D12DescriptorHeap* heaps[]{slot.descriptors.Get(),slot.sampler_heaps.back().Get()}; slot.command->SetDescriptorHeaps(2,heaps);
            bound_.textures={}; bound_.samplers={};   // changing heaps leaves the root tables unbound
        }
        const UINT first=slot.sampler_used; slot.sampler_used+=8;
        for(UINT i=0;i<8;++i) {
            const auto mode=modes[i],base=uint16_t(mode>=257 ? mode-257 : mode);
            if(mode>512 || (base<256 && ((base&3)>2 || ((base>>2)&3)>2 || ((base>>5)&3)>2))) throw std::invalid_argument("Invalid DirectX sampler mode");
            auto source=samplers_->GetCPUDescriptorHandleForHeapStart(); source.ptr+=size_t(mode)*sampler_stride_;
            auto target=slot.sampler_heaps.back()->GetCPUDescriptorHandleForHeapStart(); target.ptr+=size_t(first+i)*sampler_stride_;
            device_->CopyDescriptorsSimple(1,target,source,D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        }
        found=slot.sampler_tables.emplace(modes,first).first;
    }
    auto handle=slot.sampler_heaps.back()->GetGPUDescriptorHandleForHeapStart(); handle.ptr+=uint64_t(found->second)*sampler_stride_; return handle;
}
void D3D12Renderer::set_bloom(const BloomSettings& settings) {
    if(!valid_bloom_settings(settings)) throw std::invalid_argument("Invalid DirectX bloom settings");
    bloom_=settings;
}
void D3D12Renderer::set_dlss5(const Dlss5Settings& settings) {
    for(const auto& field:dlss5_fields()) if(const auto error=dlss5_value_error(field,field.get(settings)); !error.empty()) throw std::invalid_argument("Invalid DirectX DLSS 5 setting: "+error);
    if(recording_) { pending_dlss5_=settings; return; }
    const bool needed=wants_scene();
    dlss5_settings_=settings; dlss5_pass_.settings_changed();
    if(device_ && needed!=wants_scene()) {
        idle(); reset_scene(); invalidate_held(); scene_phase_=false; frame_start_=true; scene_restore_copy_.reset();
    }
}
bool D3D12Renderer::record_dlss5(Slot& slot,Dlss5Hook hook,const std::shared_ptr<Image>& picture,std::array<int32_t,2> origin,RenderExtent size,std::array<float,2> jitter,bool black) {
    if(!dlss5_requested() || dlss5_settings_.hook!=hook || !dlss5_scene_ready_ || size.width<8 || size.height<8 || (dlss5_settings_.block_hudless && black)) return false;
    for(const auto& image:{scene_depth_,scene_motion_}) { transition(slot,*image,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); slot.images.push_back(image); }
    transition(slot,*picture,D3D12_RESOURCE_STATE_UNORDERED_ACCESS); slot.images.push_back(picture);
    const auto resource=[](const std::shared_ptr<Image>& image) { return NativeUpscaleImage{image->resource.Get(),uint32_t(image->format),uint32_t(image->states[0]),{image->width,image->height}}; };
    NativeDlss5Frame frame; frame.command=slot.command.Get(); frame.picture=resource(picture); frame.depth=resource(scene_depth_); frame.motion=resource(scene_motion_);
    frame.origin=origin; frame.size=size; frame.scene_size=dlss5_scene_size_; frame.jitter=jitter; frame.reset=dlss5_scene_reset_; frame.settings=dlss5_settings_;
    if(auto work=dlss5_pass_.record(frame)) slot.dlss5_work.push_back(std::move(work));
    ID3D12DescriptorHeap* heaps[]{slot.descriptors.Get(),slot.sampler_heaps.back().Get()}; slot.command->SetDescriptorHeaps(2,heaps); slot.command->SetGraphicsRootSignature(root_.Get());
    return dlss5_pass_.ran();
}
bool D3D12Renderer::set_upscaling(UpscalerKind kind,UpscaleMode mode,bool sharpening,double sharpness,bool generation,uint32_t multiplier) {
    if(unsigned(kind)>unsigned(UpscalerKind::smaa) || unsigned(mode)>unsigned(UpscaleMode::ultra_performance) || !std::isfinite(sharpness) || sharpness<0.01 || sharpness>1 || multiplier<2 || multiplier>6) throw std::invalid_argument("Invalid DirectX upscaling settings");
    if(generation && kind!=UpscalerKind::dlss && kind!=UpscalerKind::fsr) throw std::invalid_argument("DirectX frame generation requires DLSS or FSR");
    if(device_ && kind==UpscalerKind::dlss && !streamline_) return false;
    if(device_ && generation && kind==UpscalerKind::dlss && !streamline_->frame_generation_loaded()) return false;
    const bool requested_rr=(pending_controls_ ? pending_controls_->effects : controls_.effects).ray_reconstruction;
    if(device_ && kind==UpscalerKind::dlss && requested_rr && !streamline_->ray_reconstruction_loaded()) return false;
    if(device_ && kind==UpscalerKind::dlss && requested_rr && !rr_supported_) throw std::runtime_error("DirectX DLSS Ray Reconstruction is unavailable on this adapter");
    const bool changed=upscaler_kind_!=kind || upscale_mode_!=mode;
    const bool generation_changed=generation_requested_!=generation || (generation && generation_multiplier_!=multiplier);
    if(upscaler_kind_!=kind || sharpening_!=sharpening || sharpness_!=sharpness || upscale_mode_!=mode) invalidate_held();
    upscaler_kind_=kind; upscale_mode_=mode; sharpening_=sharpening; sharpness_=sharpness;
    generation_requested_=generation; generation_multiplier_=multiplier;
    if(device_ && (generation_changed || (changed && generation_))) {
        generation_recreate_=generation ? !generation_ || generation_kind_!=kind || generation_->multiplier()!=multiplier : generation_ && generation_kind_!=kind;
        output_dirty_=true; invalidate_held();
    }
    if(changed && device_) upscaler_dirty_=true;
    return true;
}
void D3D12Renderer::configure_upscaler() {
    if(upscaler_kind_==UpscalerKind::dlss && !upscaler_) upscaler_=make_native_dlss_upscaler(*streamline_,upscale_mode_);
    else if(upscaler_kind_==UpscalerKind::fsr && !upscaler_) upscaler_=make_native_fsr_upscaler(device_.Get(),shaders_.parent_path().parent_path());
    const RenderExtent output{efb_->width,efb_->height};
    internal_=internal_extent(output,reconstruction() ? upscale_mode_ : UpscaleMode::native,upscaler_ ? upscaler_->minimum_internal(output) : RenderExtent{1,1});
}
void D3D12Renderer::antialias_scene(Slot& slot,bool only_sharpen) {
    if(!smaa_root_) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R8G8_UNORM};
        check(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof support),"DirectX SMAA format support");
        if(!(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) || !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) throw std::runtime_error("DirectX adapter lacks SMAA sampling/UAV support");
        D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,5,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,3,0,0,5}};
        D3D12_ROOT_PARAMETER parameters[2]{}; parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[1].DescriptorTable={2,ranges};
        std::array<D3D12_STATIC_SAMPLER_DESC,2> samplers{};
        for(UINT i=0;i<samplers.size();++i) {
            auto& sampler=samplers[i]; sampler.ShaderRegister=i; sampler.Filter=i ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.MaxAnisotropy=1; sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxLOD=D3D12_FLOAT32_MAX;
        }
        D3D12_ROOT_SIGNATURE_DESC signature{}; signature.NumParameters=2; signature.pParameters=parameters; signature.NumStaticSamplers=UINT(samplers.size()); signature.pStaticSamplers=samplers.data();
        ComPtr<ID3DBlob> blob,error;
        const auto serialized=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);
        if(FAILED(serialized) && error) throw std::runtime_error(std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()));
        check(serialized,"Serialize DirectX SMAA signature");
        ComPtr<ID3D12RootSignature> root; check(device_->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Create DirectX SMAA signature");
        const char* names[]{"smaa.dx12.edges.cso","smaa.dx12.weights.cso","smaa.dx12.blend.cso","rcas.dx12.cso"};
        std::array<ComPtr<ID3D12PipelineState>,4> pipelines;
        for(size_t i=0;i<pipelines.size();++i) {
            const auto shader=shader_bytes(shaders_/names[i]); D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature=root.Get(); desc.CS={shader.data(),shader.size()};
            const auto result=device_->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pipelines[i])); if(FAILED(result)) stats(); check(result,"Create DirectX SMAA/RCAS pipeline");
        }
        smaa_root_=std::move(root); smaa_pipelines_=std::move(pipelines); stats_.state_objects+=smaa_pipelines_.size();
    }
    if(!smaa_area_) {
        const auto lookup=[&](UINT width,UINT height,DXGI_FORMAT format,const void* bytes,UINT pitch) {
            auto result=image(width,height,1,format,false);
            const auto desc=result->resource->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT rows=0; UINT64 row_bytes=0,total=0;
            device_->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&row_bytes,&total);
            auto staging=buffer(size_t(total),D3D12_HEAP_TYPE_UPLOAD); void* mapped=nullptr; D3D12_RANGE empty{};
            check(staging->Map(0,&empty,&mapped),"Map DirectX SMAA lookup upload");
            for(UINT row=0;row<rows;++row) std::memcpy(static_cast<std::byte*>(mapped)+footprint.Offset+size_t(row)*footprint.Footprint.RowPitch,static_cast<const std::byte*>(bytes)+size_t(row)*pitch,size_t(row_bytes));
            staging->Unmap(0,nullptr); transition(slot,*result,D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource=staging.Get(); source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; source.PlacedFootprint=footprint;
            D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource=result->resource.Get(); destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            slot.command->CopyTextureRegion(&destination,0,0,0,&source,nullptr); slot.resources.push_back(staging); slot.images.push_back(result);
            return result;
        };
        smaa_area_=lookup(AREATEX_WIDTH,AREATEX_HEIGHT,DXGI_FORMAT_R8G8_UNORM,areaTexBytes,AREATEX_PITCH);
        smaa_search_=lookup(SEARCHTEX_WIDTH,SEARCHTEX_HEIGHT,DXGI_FORMAT_R8_UNORM,searchTexBytes,SEARCHTEX_PITCH);
    }
    const auto viewport=boundary_.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
    const uint32_t width=uint32_t(std::clamp<long>(std::lround((viewport[0]+viewport[2])*efb_->width/640),1,efb_->width));
    const uint32_t height=uint32_t(std::clamp<long>(std::lround((viewport[1]+viewport[3])*efb_->height/480),1,efb_->height));
    if(!smaa_source_ || smaa_source_->width!=width || smaa_source_->height!=height) {
        smaa_source_=image(width,height,1,color_format,false,false,true);
        smaa_edges_=image(width,height,1,DXGI_FORMAT_R8G8_UNORM,false,false,true);
        smaa_weights_=image(width,height,1,color_format,false,false,true);
        smaa_output_=image(width,height,1,color_format,false,false,true);
    }
    const auto copy=[&](Image& source,Image& destination) {
        transition(slot,source,D3D12_RESOURCE_STATE_COPY_SOURCE); transition(slot,destination,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource=source.resource.Get(); from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource=destination.resource.Get(); to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX region{0,0,0,width,height,1}; slot.command->CopyTextureRegion(&to,0,0,0,&from,&region);
    };
    copy(*efb_,*smaa_source_);
    const std::array<float,4> metrics{1.f/width,1.f/height,float(width),float(height)};
    const auto metrics_offset=upload(slot,metrics.data(),sizeof metrics);
    const auto dispatch=[&](UINT stage,const std::array<std::shared_ptr<Image>,5>& inputs,const std::array<std::shared_ptr<Image>,3>& outputs,size_t offset) {
        const UINT table=descriptors(slot);
        for(UINT i=0;i<inputs.size();++i) {
            if(inputs[i]) { transition(slot,*inputs[i],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); srv(slot,table+i,*inputs[i]); slot.images.push_back(inputs[i]); }
            else {
                D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=color_format; view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels=1;
                device_->CreateShaderResourceView(nullptr,&view,cpu(slot,table+i));
            }
        }
        for(UINT i=0;i<outputs.size();++i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D; view.Format=outputs[i] ? outputs[i]->format : (i==0 && stage!=3 ? DXGI_FORMAT_R8G8_UNORM : color_format);
            if(outputs[i]) { transition(slot,*outputs[i],D3D12_RESOURCE_STATE_UNORDERED_ACCESS); slot.images.push_back(outputs[i]); }
            device_->CreateUnorderedAccessView(outputs[i] ? outputs[i]->resource.Get() : nullptr,nullptr,&view,cpu(slot,table+5+i));
        }
        slot.command->SetComputeRootSignature(smaa_root_.Get()); slot.command->SetComputeRootConstantBufferView(0,slot.upload->GetGPUVirtualAddress()+offset);
        slot.command->SetComputeRootDescriptorTable(1,gpu(slot,table)); slot.command->SetPipelineState(smaa_pipelines_[stage].Get()); slot.command->Dispatch((width+7)/8,(height+7)/8,1);
    };
    if(!only_sharpen) {
        dispatch(0,{smaa_source_,nullptr,nullptr,nullptr,nullptr},{smaa_edges_,nullptr,nullptr},metrics_offset);
        dispatch(1,{nullptr,smaa_edges_,nullptr,smaa_area_,smaa_search_},{nullptr,smaa_weights_,nullptr},metrics_offset);
        dispatch(2,{smaa_source_,nullptr,smaa_weights_,nullptr,nullptr},{nullptr,nullptr,smaa_output_},metrics_offset);
    }
    if(sharpening_) {
        struct Constants { int32_t size[2]; float sharpness,unused; };
        static_assert(sizeof(Constants)==16);
        const Constants constants{{int32_t(width),int32_t(height)},rcas_constant(sharpness_),0};
        dispatch(3,{only_sharpen ? smaa_source_ : smaa_output_,nullptr,nullptr,nullptr,nullptr},{only_sharpen ? smaa_output_ : smaa_source_,nullptr,nullptr},upload(slot,&constants,sizeof constants));
    }
    copy(sharpening_ && !only_sharpen ? *smaa_source_ : *smaa_output_,*efb_);
}
void D3D12Renderer::initialize_bloom() {
    if(bloom_root_) return;
    for(const auto format:{DXGI_FORMAT_R16G16B16A16_FLOAT,color_format}) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
        check(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof support),"DirectX bloom format support");
        if(!(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) || !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) throw std::runtime_error("DirectX adapter lacks bloom sampling/UAV support");
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,2}};
    D3D12_ROOT_PARAMETER parameters[2]{}; parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[1].DescriptorTable={2,ranges};
    D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR; sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy=1; sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxLOD=D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC signature{}; signature.NumParameters=2; signature.pParameters=parameters; signature.NumStaticSamplers=1; signature.pStaticSamplers=&sampler;
    ComPtr<ID3DBlob> blob,error;
    const auto serialized=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);
    if(FAILED(serialized) && error) throw std::runtime_error(std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()));
    check(serialized,"Serialize DirectX bloom signature");
    ComPtr<ID3D12RootSignature> root; check(device_->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Create DirectX bloom signature");
    const char* names[]{"bloom.dx12.down_first.cso","bloom.dx12.down.cso","bloom.dx12.up.cso","bloom.dx12.composite.cso"};
    std::array<ComPtr<ID3D12PipelineState>,4> pipelines;
    for(size_t i=0;i<pipelines.size();++i) {
        const auto shader=shader_bytes(shaders_/names[i]); D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature=root.Get(); desc.CS={shader.data(),shader.size()};
        const auto result=device_->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pipelines[i]));
        if(FAILED(result)) stats(); check(result,"Create DirectX bloom pipeline");
    }
    bloom_root_=std::move(root); bloom_pipelines_=std::move(pipelines); stats_.state_objects+=bloom_pipelines_.size();
}
void D3D12Renderer::bloom_dispatch(Slot& slot,UINT stage,const std::shared_ptr<Image>& a,const std::shared_ptr<Image>& b,const std::shared_ptr<Image>& output,const BloomParams& params) {
    transition(slot,*a,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(slot,*b,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(slot,*output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    slot.images.push_back(a); slot.images.push_back(b); slot.images.push_back(output);
    const UINT table=descriptors(slot); srv(slot,table,*a); srv(slot,table+1,*b);
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D; view.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    device_->CreateUnorderedAccessView(stage==3 ? nullptr : output->resource.Get(),nullptr,&view,cpu(slot,table+2));
    view.Format=color_format; device_->CreateUnorderedAccessView(stage==3 ? output->resource.Get() : nullptr,nullptr,&view,cpu(slot,table+3));
    const auto offset=upload(slot,&params,sizeof params);
    slot.command->SetComputeRootSignature(bloom_root_.Get()); slot.command->SetComputeRootConstantBufferView(0,slot.upload->GetGPUVirtualAddress()+offset);
    slot.command->SetComputeRootDescriptorTable(1,gpu(slot,table)); slot.command->SetPipelineState(bloom_pipelines_[stage].Get());
    const auto width=stage==3 ? params.region[0] : params.dst[0],height=stage==3 ? params.region[1] : params.dst[1];
    slot.command->Dispatch((width+7)/8,(height+7)/8,1);
}
void D3D12Renderer::initialize_effects() {
    if(effects_root_) return;
    for(const auto format:{DXGI_FORMAT_R8_UNORM,DXGI_FORMAT_R16G16_FLOAT,color_format}) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format}; check(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof support),"DirectX effects format support");
        if(!(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) || !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) throw std::runtime_error("DirectX adapter lacks effects sampling/UAV support");
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,7,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,4,0,0,7}};
    D3D12_ROOT_PARAMETER parameters[3]{};
    for(UINT i=0;i<2;++i) { parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[i].Descriptor.ShaderRegister=i; }
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[2].DescriptorTable={2,ranges};
    D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR; sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy=1; sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxLOD=D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC signature{}; signature.NumParameters=3; signature.pParameters=parameters; signature.NumStaticSamplers=1; signature.pStaticSamplers=&sampler;
    ComPtr<ID3DBlob> blob,error; const auto serialized=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);
    if(FAILED(serialized) && error) throw std::runtime_error(std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()));
    check(serialized,"Serialize DirectX effects signature");
    ComPtr<ID3D12RootSignature> root; check(device_->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Create DirectX effects signature");
    const char* names[]{"gtao.dx12.ao.cso","gtao.dx12.denoise.cso","gtao.dx12.contact.cso","gtao.dx12.composite.cso","motion_blur.dx12.tile.cso","motion_blur.dx12.blur.cso"};
    std::array<ComPtr<ID3D12PipelineState>,6> pipelines;
    for(size_t i=0;i<pipelines.size();++i) {
        const auto shader=shader_bytes(shaders_/names[i]); D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature=root.Get(); desc.CS={shader.data(),shader.size()};
        const auto result=device_->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pipelines[i])); if(FAILED(result)) stats(); check(result,"Create DirectX effects pipeline");
    }
    effects_root_=std::move(root); effects_pipelines_=std::move(pipelines); stats_.state_objects+=effects_pipelines_.size();
}
void D3D12Renderer::effect_dispatch(Slot& slot,UINT stage,const void* params,size_t size,const std::array<std::shared_ptr<Image>,7>& inputs,const std::array<std::shared_ptr<Image>,4>& outputs,uint32_t width,uint32_t height) {
    const UINT table=descriptors(slot);
    for(UINT i=0;i<inputs.size();++i) {
        if(inputs[i]) { transition(slot,*inputs[i],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); srv(slot,table+i,*inputs[i]); slot.images.push_back(inputs[i]); }
        else {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=color_format; view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels=1;
            device_->CreateShaderResourceView(nullptr,&view,cpu(slot,table+i));
        }
    }
    for(UINT i=0;i<outputs.size();++i) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D; view.Format=outputs[i] ? outputs[i]->format : (i==0 ? color_format : stage>=4 && i==1 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R8_UNORM);
        if(outputs[i]) { transition(slot,*outputs[i],D3D12_RESOURCE_STATE_UNORDERED_ACCESS); slot.images.push_back(outputs[i]); }
        device_->CreateUnorderedAccessView(outputs[i] ? outputs[i]->resource.Get() : nullptr,nullptr,&view,cpu(slot,table+7+i));
    }
    const auto offset=upload(slot,params,size); NativeShadowBlock shadow{};
    if(shadow_valid_ && shadow_fit_ && dominant_light_) {
        const auto preset=shadow_preset(effects_.shadows);
        for(UINT row=0;row<3;++row) for(UINT col=0;col<4;++col) shadow.rows[row][col]=shadow_fit_->view_to_light[row][col];
        shadow.info[0]=float(preset.resolution); shadow.info[1]=shadow_fit_->texel_world; shadow.info[2]=shadow_fit_->depth_range; shadow.info[3]=0.04f;
        shadow.taps[0]=preset.filter_taps; shadow.taps[1]=preset.blocker_taps; shadow.taps[2]=preset.soft ? 1 : 0; shadow.taps[3]=1;
        const auto& p=dominant_light_->position; const float length=std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
        for(UINT i=0;i<3;++i) shadow.to_light[i]=p[i]/length;
    }
    const auto shadow_offset=upload(slot,&shadow,sizeof shadow);
    slot.command->SetComputeRootSignature(effects_root_.Get()); slot.command->SetComputeRootConstantBufferView(0,slot.upload->GetGPUVirtualAddress()+offset); slot.command->SetComputeRootConstantBufferView(1,slot.upload->GetGPUVirtualAddress()+shadow_offset);
    slot.command->SetComputeRootDescriptorTable(2,gpu(slot,table)); slot.command->SetPipelineState(effects_pipelines_[stage].Get()); slot.command->Dispatch((width+7)/8,(height+7)/8,1);
}
void D3D12Renderer::occlusion(Slot& slot) {
    const auto preset=ao_preset(effects_.ambient_occlusion);
    const bool contact=effects_.contact_shadows && dominant_light_.has_value();
    if(!frame_projection_ || (!preset.slices && !contact && !shadow_valid_)) return;
    initialize_effects();
    const auto color=raster_color_ ? raster_color_ : efb_;
    const auto viewport=boundary_.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
    const uint32_t width=uint32_t(std::clamp<long>(std::lround((viewport[0]+viewport[2])*color->width/640),1,color->width));
    const uint32_t height=uint32_t(std::clamp<long>(std::lround((viewport[1]+viewport[3])*color->height/480),1,color->height));
    const uint32_t half_width=(width+1)/2,half_height=(height+1)/2,ao_width=preset.full_resolution ? width : half_width,ao_height=preset.full_resolution ? height : half_height;
    if(!ao_raw_ || ao_raw_->width!=ao_width || ao_raw_->height!=ao_height) { ao_raw_=image(ao_width,ao_height,1,DXGI_FORMAT_R8_UNORM,false,false,true); ao_filtered_=image(ao_width,ao_height,1,DXGI_FORMAT_R8_UNORM,false,false,true); }
    if(!contact_ || contact_->width!=half_width || contact_->height!=half_height) contact_=image(half_width,half_height,1,DXGI_FORMAT_R8_UNORM,false,false,true);
    if(!ao_output_) ao_output_=image(color->width,color->height,1,color_format,false,false,true);
    transition(slot,*scene_color_,D3D12_RESOURCE_STATE_COPY_SOURCE); transition(slot,*ao_output_,D3D12_RESOURCE_STATE_COPY_DEST); slot.command->CopyResource(ao_output_->resource.Get(),scene_color_->resource.Get());
    NativeOcclusionParams params{}; const auto view=view_params(*frame_projection_);
    params.viewport[0]=viewport[0]*color->width/640; params.viewport[1]=viewport[1]*color->height/480; params.viewport[2]=viewport[2]*color->width/640; params.viewport[3]=viewport[3]*color->height/480;
    params.view_a[0]=view.x_scale; params.view_a[1]=view.y_scale; params.view_a[2]=view.x_offset; params.view_a[3]=view.y_offset; params.view_b[0]=view.depth_a; params.view_b[1]=view.depth_b;
    params.region[0]=width; params.region[1]=height; params.ao_size[0]=ao_width; params.ao_size[1]=ao_height; params.slices=preset.slices; params.steps=preset.steps;
    params.depth_range[0]=viewport[4]; params.depth_range[1]=viewport[5]; params.contact_size[0]=half_width; params.contact_size[1]=half_height; params.flags=(preset.slices ? 1 : 0)|(contact ? 2 : 0)|(shadow_valid_ ? 4 : 0);
    if(contact) { for(UINT i=0;i<3;++i) params.light[i]=dominant_light_->position[i]; params.light[3]=dominant_light_->kind==gx::LightKind::spot ? 1.f : 0.f; }
    if(preset.slices) {
        effect_dispatch(slot,0,&params,sizeof params,{scene_depth_,scene_gbuffer_,nullptr,nullptr,nullptr,nullptr,nullptr},{nullptr,ao_raw_,nullptr,nullptr},ao_width,ao_height);
        effect_dispatch(slot,1,&params,sizeof params,{scene_depth_,scene_gbuffer_,ao_raw_,nullptr,nullptr,nullptr,nullptr},{nullptr,nullptr,ao_filtered_,nullptr},ao_width,ao_height);
    }
    if(contact) effect_dispatch(slot,2,&params,sizeof params,{scene_depth_,scene_gbuffer_,nullptr,nullptr,nullptr,nullptr,nullptr},{nullptr,nullptr,nullptr,contact_},half_width,half_height);
    effect_dispatch(slot,3,&params,sizeof params,{scene_depth_,scene_gbuffer_,nullptr,ao_filtered_,shadow_valid_ ? shadow_map_ : scene_depth_,contact_,scene_color_},{ao_output_,nullptr,nullptr,nullptr},width,height);
    transition(slot,*ao_output_,D3D12_RESOURCE_STATE_COPY_SOURCE); transition(slot,*color,D3D12_RESOURCE_STATE_COPY_DEST); slot.command->CopyResource(color->resource.Get(),ao_output_->resource.Get());
    transition(slot,*scene_color_,D3D12_RESOURCE_STATE_COPY_DEST); slot.command->CopyResource(scene_color_->resource.Get(),ao_output_->resource.Get());
}
void D3D12Renderer::motion_blur(Slot& slot) {
    const auto preset=blur_preset(effects_.motion_blur); if(!preset.samples) return;
    initialize_effects();
    const auto viewport=boundary_.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
    const uint32_t width=uint32_t(std::clamp<long>(std::lround((viewport[0]+viewport[2])*efb_->width/640),1,efb_->width));
    const uint32_t height=uint32_t(std::clamp<long>(std::lround((viewport[1]+viewport[3])*efb_->height/480),1,efb_->height));
    const uint32_t render_width=uint32_t(std::clamp<long>(std::lround((viewport[0]+viewport[2])*scene_motion_->width/640),1,scene_motion_->width));
    const uint32_t render_height=uint32_t(std::clamp<long>(std::lround((viewport[1]+viewport[3])*scene_motion_->height/480),1,scene_motion_->height));
    const uint32_t tile_width=(render_width+15)/16,tile_height=(render_height+15)/16;
    if(!effect_output_) effect_output_=image(efb_->width,efb_->height,1,color_format,false,false,true);
    if(!effect_source_) effect_source_=image(efb_->width,efb_->height,1,color_format,false);
    if(!blur_tiles_ || blur_tiles_->width!=tile_width || blur_tiles_->height!=tile_height) blur_tiles_=image(tile_width,tile_height,1,DXGI_FORMAT_R16G16_FLOAT,false,false,true);
    transition(slot,*efb_,D3D12_RESOURCE_STATE_COPY_SOURCE);
    for(const auto& target:{effect_source_,effect_output_}) { transition(slot,*target,D3D12_RESOURCE_STATE_COPY_DEST); slot.command->CopyResource(target->resource.Get(),efb_->resource.Get()); }
    const NativeBlurParams params{{width,height},{render_width,render_height},preset.shutter,preset.samples,16,0};
    effect_dispatch(slot,4,&params,sizeof params,{nullptr,scene_motion_,nullptr,nullptr,nullptr,nullptr,nullptr},{nullptr,blur_tiles_,nullptr,nullptr},tile_width,tile_height);
    effect_dispatch(slot,5,&params,sizeof params,{effect_source_,scene_motion_,blur_tiles_,nullptr,nullptr,nullptr,nullptr},{effect_output_,nullptr,nullptr,nullptr},width,height);
    transition(slot,*effect_output_,D3D12_RESOURCE_STATE_COPY_SOURCE); transition(slot,*efb_,D3D12_RESOURCE_STATE_COPY_DEST); slot.command->CopyResource(efb_->resource.Get(),effect_output_->resource.Get());
}
void D3D12Renderer::copy_depth(Slot& slot,RenderExtent source,RenderExtent target) {
    if(!depth_blit_pipeline_) {
        const auto vertex=shader_bytes(shaders_/"depth.dx12.vert.cso"),pixel=shader_bytes(shaders_/"depth.dx12.frag.cso");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{}; desc.pRootSignature=root_.Get(); desc.VS={vertex.data(),vertex.size()}; desc.PS={pixel.data(),pixel.size()};
        desc.SampleMask=UINT_MAX; desc.SampleDesc.Count=1; desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; desc.DSVFormat=DXGI_FORMAT_D32_FLOAT;
        desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; desc.RasterizerState.DepthClipEnable=TRUE;
        desc.DepthStencilState.DepthEnable=TRUE; desc.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL; desc.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        const auto result=device_->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&depth_blit_pipeline_)); if(FAILED(result)) stats(); check(result,"Create DirectX depth scaling pipeline"); ++stats_.state_objects;
    }
    transition(slot,*scene_depth_,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); transition(slot,*depth_,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const auto dsv=depth_->views->GetCPUDescriptorHandleForHeapStart(); slot.command->OMSetRenderTargets(0,nullptr,FALSE,&dsv);
    const D3D12_VIEWPORT viewport{0,0,float(target.width),float(target.height),0,1}; const D3D12_RECT scissor{0,0,LONG(target.width),LONG(target.height)};
    slot.command->RSSetViewports(1,&viewport); slot.command->RSSetScissorRects(1,&scissor);
    const std::array<uint32_t,4> constants{source.width,source.height,target.width,target.height}; const auto offset=upload(slot,constants.data(),sizeof constants);
    const auto table=descriptors(slot); srv(slot,table,*scene_depth_);
    slot.command->SetGraphicsRootConstantBufferView(0,slot.upload->GetGPUVirtualAddress()+offset); slot.command->SetGraphicsRootDescriptorTable(2,gpu(slot,table));
    slot.command->SetPipelineState(depth_blit_pipeline_.Get()); slot.command->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); slot.command->DrawInstanced(3,1,0,0);
}
void D3D12Renderer::reconstruct_scene(Slot& slot,bool black) {
    if(!raster_color_) return;
    const auto viewport=boundary_.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
    const auto region=[&](const Image& image) { return RenderExtent{uint32_t(std::clamp<long>(std::lround((viewport[0]+viewport[2])*image.width/640),1,image.width)),uint32_t(std::clamp<long>(std::lround((viewport[1]+viewport[3])*image.height/480),1,image.height))}; };
    const auto render=region(*scene_color_),output=region(*efb_);
    if(upscaler_ && !black) {
        if(last_sdk_render_.width && (last_sdk_render_.width!=render.width || last_sdk_render_.height!=render.height || last_sdk_output_.width!=output.width || last_sdk_output_.height!=output.height)) idle();
        for(const auto& image:{scene_color_,scene_depth_,scene_motion_}) { transition(slot,*image,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); slot.images.push_back(image); }
        transition(slot,*efb_,D3D12_RESOURCE_STATE_UNORDERED_ACCESS); slot.images.push_back(efb_);
        const auto resource=[](const std::shared_ptr<Image>& image) { return NativeUpscaleImage{image->resource.Get(),uint32_t(image->format),uint32_t(image->states[0]),{image->width,image->height}}; };
        NativeUpscaleInputs inputs; inputs.command=slot.command.Get(); inputs.color=resource(scene_color_); inputs.depth=resource(scene_depth_); inputs.depth.format=uint32_t(DXGI_FORMAT_D32_FLOAT); inputs.motion=resource(scene_motion_); inputs.output=resource(efb_);
        inputs.render_size=render; inputs.output_size=output; inputs.jitter=jitter_; inputs.reset=reset_next_scene_ || tracker_->camera_cut() || !motion_cleared_;
        inputs.sharpening=sharpening_; inputs.sharpness=float(sharpness_); inputs.projection=scene_projection_; inputs.clip_to_prev_clip=tracker_->clip_to_prev_clip();
        if(rr_active() && rt_work_ && rt_work_->normal_roughness.resource && rt_work_->specular_albedo.resource) {
            transition(slot,*scene_albedo_,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            inputs.ray_reconstruction=true; inputs.diffuse_albedo=resource(scene_albedo_);
            inputs.normal_roughness=rt_work_->normal_roughness; inputs.specular_albedo=rt_work_->specular_albedo;
            slot.rt_work.push_back(rt_work_); slot.images.push_back(scene_albedo_);
        }
        const auto now=Clock::now(); inputs.frame_ms=last_upscale_==Clock::time_point{} ? 16.7f : std::clamp(std::chrono::duration<float,std::milli>(now-last_upscale_).count(),0.1f,100.f);
        upscaler_->evaluate(inputs); last_upscale_=now; last_sdk_render_=render; last_sdk_output_=output;
        if(frame_generation_active() && generation_->active()) { generation_->prepare(inputs); generation_prepared_=true; }
        ID3D12DescriptorHeap* heaps[]{slot.descriptors.Get(),slot.sampler_heaps.back().Get()}; slot.command->SetDescriptorHeaps(2,heaps); slot.command->SetGraphicsRootSignature(root_.Get());
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; barrier.UAV.pResource=efb_->resource.Get(); slot.command->ResourceBarrier(1,&barrier);
    } else blit(slot,*scene_color_,0,*efb_,0,{0,0,render.width/float(scene_color_->width),render.height/float(scene_color_->height)},0,true,output);
    copy_depth(slot,render,output);
    if(sharpening_ && (!upscaler_ || !upscaler_->sharpens()) && !black) antialias_scene(slot,true);
}
void D3D12Renderer::record_rt_geometry(Slot& slot) {
    if(rt_geometry_.empty()) return;
    ComPtr<ID3D12GraphicsCommandList4> command; check(slot.command.As(&command),"DirectX ray tracing command list");
    rt_scene_.record_packet(command.Get(),rt_geometry_); rt_geometry_.clear();
    slot.rt_frames.push_back(rt_scene_.frame());
    ID3D12DescriptorHeap* heaps[]{slot.descriptors.Get(),slot.sampler_heaps.back().Get()}; slot.command->SetDescriptorHeaps(2,heaps); slot.command->SetGraphicsRootSignature(root_.Get());
}
bool D3D12Renderer::ray_trace(Slot& slot,bool black) {
    rt_work_.reset();
    if(!rt_active() || black || !frame_projection_) return false;
    record_rt_geometry(slot);
    ComPtr<ID3D12GraphicsCommandList4> command; check(slot.command.As(&command),"DirectX ray tracing command list");
    if(!rt_scene_.build_tlas(command.Get())) return false;
    if(!rt_effects_ && rt_effects_warm_.valid()) rt_effects_=rt_effects_warm_.get();
    if(!rt_effects_) rt_effects_=std::make_unique<NativeRtEffects>(device_.Get(),shaders_);
    slot.rt_frames.push_back(rt_scene_.frame());
    for(const auto& image:{scene_depth_,scene_motion_,scene_gbuffer_,scene_material_,scene_albedo_}) { transition(slot,*image,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); slot.images.push_back(image); }
    transition(slot,*scene_color_,D3D12_RESOURCE_STATE_UNORDERED_ACCESS); slot.images.push_back(scene_color_);
    const auto resource=[](const std::shared_ptr<Image>& image) { return NativeUpscaleImage{image->resource.Get(),uint32_t(image->format),uint32_t(image->states[0]),{image->width,image->height}}; };
    NativeRtInputs inputs; inputs.command=slot.command.Get(); inputs.scene=rt_scene_.frame();
    inputs.color=resource(scene_color_); inputs.depth=resource(scene_depth_); inputs.motion=resource(scene_motion_); inputs.gbuffer=resource(scene_gbuffer_); inputs.material=resource(scene_material_); inputs.albedo=resource(scene_albedo_);
    inputs.records=rt_records_; inputs.lights=rt_lights_; inputs.effects=effects_; inputs.view=view_params(*frame_projection_);
    const auto viewport=boundary_.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
    inputs.region={uint32_t(std::clamp<long>(std::lround((viewport[0]+viewport[2])*scene_color_->width/640),1,scene_color_->width)),uint32_t(std::clamp<long>(std::lround((viewport[1]+viewport[3])*scene_color_->height/480),1,scene_color_->height))};
    inputs.viewport={viewport[0]*scene_color_->width/640,viewport[1]*scene_color_->height/480,viewport[2]*scene_color_->width/640,viewport[3]*scene_color_->height/480,viewport[4],viewport[5]};
    rt_overhead_=tracker_->camera_cut() ? kRtOverheadDirection : rt_key_direction_after(rt_overhead_,tracker_->view_to_prev_view());
    inputs.overhead=rt_overhead_; inputs.frame=uint32_t(upscale_frame_); inputs.reset=reset_next_scene_ || tracker_->camera_cut(); inputs.ray_reconstruction=rr_active();
    inputs.raster_occlusion=[&] {
        ID3D12DescriptorHeap* heaps[]{slot.descriptors.Get(),slot.sampler_heaps.back().Get()}; slot.command->SetDescriptorHeaps(2,heaps); slot.command->SetGraphicsRootSignature(root_.Get());
        occlusion(slot);
        for(const auto& image:{scene_depth_,scene_motion_,scene_gbuffer_,scene_material_,scene_albedo_}) transition(slot,*image,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(slot,*scene_color_,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };
    rt_work_=rt_effects_->execute(inputs); slot.rt_work.push_back(rt_work_);
    ID3D12DescriptorHeap* heaps[]{slot.descriptors.Get(),slot.sampler_heaps.back().Get()}; slot.command->SetDescriptorHeaps(2,heaps); slot.command->SetGraphicsRootSignature(root_.Get());
    if(!raster_color_) {
        transition(slot,*scene_color_,D3D12_RESOURCE_STATE_COPY_SOURCE); transition(slot,*efb_,D3D12_RESOURCE_STATE_COPY_DEST);
        slot.command->CopyResource(efb_->resource.Get(),scene_color_->resource.Get());
    }
    return true;
}
void D3D12Renderer::record_bloom(Slot& slot,bool black) {
    bloom_valid_=false;
    if(wants_scene()) {
        tracker_->end_frame(); const auto light=gx::select_dominant(scene_lights_.lights(),dominant_light_);
        shadow_valid_=shadow_cleared_ && shadow_fit_.has_value() && !light_changed(dominant_light_,light) && !reset_next_scene_ && !tracker_->camera_cut() && !black;
        dominant_light_=light; snapshot_scene(slot);
        if(!black && !ray_trace(slot,false)) occlusion(slot);
        const auto viewport=boundary_.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
        dlss5_scene_size_={uint32_t(std::clamp<long>(std::lround((viewport[0]+viewport[2])*scene_color_->width/640),1,scene_color_->width)),uint32_t(std::clamp<long>(std::lround((viewport[1]+viewport[3])*scene_color_->height/480),1,scene_color_->height))};
        dlss5_scene_ready_=true; dlss5_scene_reset_=reset_next_scene_ || tracker_->camera_cut() || black || !motion_cleared_;
        if(record_dlss5(slot,Dlss5Hook::render,scene_color_,{0,0},dlss5_scene_size_,jitter_,black) && !raster_color_) {
            transition(slot,*scene_color_,D3D12_RESOURCE_STATE_COPY_SOURCE); transition(slot,*efb_,D3D12_RESOURCE_STATE_COPY_DEST);
            slot.command->CopyResource(efb_->resource.Get(),scene_color_->resource.Get());
        }
    }
    reconstruct_scene(slot,black);
    if(upscaler_kind_==UpscalerKind::smaa && !black) antialias_scene(slot);
    if(wants_scene() && !reset_next_scene_ && !tracker_->camera_cut() && !black && motion_cleared_) motion_blur(slot);
    if(dlss5_scene_ready_) {
        const auto viewport=boundary_.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
        const RenderExtent size{uint32_t(std::clamp<long>(std::lround((viewport[0]+viewport[2])*efb_->width/640),1,efb_->width)),uint32_t(std::clamp<long>(std::lround((viewport[1]+viewport[3])*efb_->height/480),1,efb_->height))};
        record_dlss5(slot,Dlss5Hook::upscaled,efb_,{0,0},size,{0,0},black);
    }
    reset_next_scene_=black; ++upscale_frame_;
    if(bloom_mix(bloom_,hdr_active_)>0 && !black) {
        initialize_bloom();
        const auto viewport=boundary_.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
        const uint32_t width=uint32_t(std::clamp<long>(std::lround((viewport[0]+viewport[2])*efb_->width/640),1,efb_->width));
        const uint32_t height=uint32_t(std::clamp<long>(std::lround((viewport[1]+viewport[3])*efb_->height/480),1,efb_->height));
        if(bloom_region_.width!=width || bloom_region_.height!=height) {
            for(UINT i=0;i<bloom_down_.size();++i) {
                const auto extent=bloom_level_extent(width,height,i);
                bloom_down_[i]=image(extent.width,extent.height,1,DXGI_FORMAT_R16G16B16A16_FLOAT,false,false,true);
                if(i<bloom_up_.size()) bloom_up_[i]=image(extent.width,extent.height,1,DXGI_FORMAT_R16G16B16A16_FLOAT,false,false,true);
            }
            bloom_region_={width,height};
        }
        if(!hdr_active_ && (!bloom_color_ || bloom_color_->width!=width || bloom_color_->height!=height)) bloom_color_=image(width,height,1,color_format,false,false,true);
        BloomParams params; params.region[0]=width; params.region[1]=height;
        params.headroom=hdr_active_ ? std::max(1.f,float(hdr_.peak_nits)/hdr_.paper_white_nits) : kBloomSdrHeadroom;
        params.src[0]=efb_->width; params.src[1]=efb_->height; params.dst[0]=bloom_down_[0]->width; params.dst[1]=bloom_down_[0]->height;
        bloom_dispatch(slot,0,efb_,efb_,bloom_down_[0],params);
        for(UINT i=1;i<bloom_down_.size();++i) {
            params.src[0]=bloom_down_[i-1]->width; params.src[1]=bloom_down_[i-1]->height; params.dst[0]=bloom_down_[i]->width; params.dst[1]=bloom_down_[i]->height;
            bloom_dispatch(slot,1,bloom_down_[i-1],bloom_down_[i-1],bloom_down_[i],params);
        }
        auto coarse=bloom_down_.back();
        for(int i=int(bloom_up_.size())-1;i>=0;--i) {
            params.src[0]=coarse->width; params.src[1]=coarse->height; params.dst[0]=bloom_up_[i]->width; params.dst[1]=bloom_up_[i]->height;
            params.weight=kBloomWeights[i]; params.weight_a=i==int(bloom_up_.size())-1 ? kBloomWeights.back() : 1.f; params.scale=i==0 ? bloom_mix(bloom_,hdr_active_) : 1.f;
            bloom_dispatch(slot,2,coarse,bloom_down_[i],bloom_up_[i],params); coarse=bloom_up_[i];
        }
        if(hdr_active_) bloom_valid_=true;
        else {
            bloom_dispatch(slot,3,bloom_up_[0],efb_,bloom_color_,params);
            transition(slot,*bloom_color_,D3D12_RESOURCE_STATE_COPY_SOURCE); transition(slot,*efb_,D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource=bloom_color_->resource.Get(); source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource=efb_->resource.Get(); destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            slot.command->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
        }
    }
    scene_phase_=false;
    if(scene_restore_copy_) { copy_texture(slot,*scene_restore_copy_); scene_restore_copy_.reset(); }
}
void D3D12Renderer::reset_scene() {
    dlss5_scene_ready_=false; dlss5_scene_reset_=true; dlss5_scene_size_={};
    scene_material_.reset(); scene_albedo_.reset(); rt_geometry_.clear(); rt_records_.clear(); rt_lights_.clear(); rt_work_.reset(); rt_overhead_=kRtOverheadDirection;
    scene_color_.reset(); scene_depth_.reset(); scene_motion_.reset(); scene_gbuffer_.reset();
    scene_captured_=motion_cleared_=false; tracker_.reset();
    scene_lights_.clear(); dominant_light_.reset(); frame_projection_.reset(); reset_next_scene_=true;
    ao_raw_.reset(); ao_filtered_.reset(); contact_.reset(); effect_output_.reset(); effect_source_.reset(); blur_tiles_.reset();
    shadow_map_.reset(); shadow_fit_.reset(); shadow_cleared_=shadow_valid_=false;
    raster_color_.reset(); raster_depth_.reset(); ao_output_.reset(); raster_initialized_=false; scene_clear_={}; jitter_={}; last_sdk_render_={}; last_sdk_output_={}; last_upscale_={};
}
void D3D12Renderer::scene_targets(Slot& slot) {
    if(!tracker_) tracker_=std::make_unique<gx::MotionTracker>();
    if(!scene_color_) {
        configure_upscaler();
        if(motion_vertex_.empty()) { motion_vertex_=shader_bytes(shaders_/"smoke.motion.dx12.vert.cso"); motion_pixel_=shader_bytes(shaders_/"smoke.motion.dx12.frag.cso"); }
        const auto size=internal_;
        scene_color_=image(size.width,size.height,1,color_format,false,false,true);
        scene_depth_=image(size.width,size.height,1,DXGI_FORMAT_D32_FLOAT,false,true);
        scene_motion_=image(size.width,size.height,1,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
        scene_gbuffer_=image(size.width,size.height,1,DXGI_FORMAT_R16G16B16A16_UNORM,true);
        if(rt_active()) {
            scene_material_=image(size.width,size.height,1,DXGI_FORMAT_R32_UINT,true,false,true);
            scene_albedo_=image(size.width,size.height,1,color_format,true);
        }
        if(reconstruction()) { raster_color_=image(size.width,size.height,1,color_format,true); raster_depth_=image(size.width,size.height,1,DXGI_FORMAT_D32_FLOAT,false,true); }
    }
    if(!motion_cleared_) {
        transition(slot,*scene_motion_,D3D12_RESOURCE_STATE_RENDER_TARGET); transition(slot,*scene_gbuffer_,D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float zero[4]{}; slot.command->ClearRenderTargetView(rtv(*scene_motion_),zero,0,nullptr); slot.command->ClearRenderTargetView(rtv(*scene_gbuffer_),zero,0,nullptr); motion_cleared_=true;
        if(scene_material_) {
            transition(slot,*scene_material_,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const UINT table=descriptors(slot); D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=DXGI_FORMAT_R32_UINT; view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            auto cpu_view=heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1);
            device_->CreateUnorderedAccessView(scene_material_->resource.Get(),nullptr,&view,cpu_view->GetCPUDescriptorHandleForHeapStart());
            device_->CopyDescriptorsSimple(1,cpu(slot,table),cpu_view->GetCPUDescriptorHandleForHeapStart(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            const UINT empty[4]{kRtNoRecord,kRtNoRecord,kRtNoRecord,kRtNoRecord};
            slot.command->ClearUnorderedAccessViewUint(gpu(slot,table),cpu_view->GetCPUDescriptorHandleForHeapStart(),scene_material_->resource.Get(),empty,0,nullptr);
            transition(slot,*scene_material_,D3D12_RESOURCE_STATE_RENDER_TARGET);
            transition(slot,*scene_albedo_,D3D12_RESOURCE_STATE_RENDER_TARGET); slot.command->ClearRenderTargetView(rtv(*scene_albedo_),zero,0,nullptr);
        }
    }
    slot.images.push_back(scene_motion_); slot.images.push_back(scene_gbuffer_);
    if(scene_material_) { slot.images.push_back(scene_material_); slot.images.push_back(scene_albedo_); }
    if(raster_color_) { slot.images.push_back(raster_color_); slot.images.push_back(raster_depth_); }
}
void D3D12Renderer::snapshot_scene(Slot& slot) {
    scene_targets(slot);
    for(const auto& pair:{std::pair{raster_color_ ? raster_color_ : efb_,scene_color_},std::pair{raster_depth_ ? raster_depth_ : depth_,scene_depth_}}) {
        transition(slot,*pair.first,D3D12_RESOURCE_STATE_COPY_SOURCE); transition(slot,*pair.second,D3D12_RESOURCE_STATE_COPY_DEST);
        slot.command->CopyResource(pair.second->resource.Get(),pair.first->resource.Get()); slot.images.push_back(pair.second);
    }
    scene_captured_=true;
}
void D3D12Renderer::set_hdr(const HdrSettings& settings) {
    if(!valid_hdr_settings(settings)) throw std::invalid_argument("Invalid DirectX HDR settings");
    if(hdr_.enabled!=settings.enabled) { output_dirty_=true; invalidate_held(); }
    if(hdr_!=settings) hdr_metadata_dirty_=true;
    if(generation_ && hdr_!=settings) output_dirty_=true;
    hdr_=settings;
}
void D3D12Renderer::hdr_metadata() {
    if(!hdr_metadata_dirty_) return;
    ComPtr<IDXGISwapChain4> chain;
    if(SUCCEEDED(swapchain_.As(&chain))) {
        if(hdr_active_) {
            DXGI_HDR_METADATA_HDR10 metadata{};
            metadata.RedPrimary[0]=35400; metadata.RedPrimary[1]=14600;
            metadata.GreenPrimary[0]=8500; metadata.GreenPrimary[1]=39850;
            metadata.BluePrimary[0]=6550; metadata.BluePrimary[1]=2300;
            metadata.WhitePoint[0]=15635; metadata.WhitePoint[1]=16450;
            // BT.2020/D65; DXGI's maximum mastering luminance uses whole nits. MaxFALL is unknown.
            metadata.MaxMasteringLuminance=std::max(hdr_.peak_nits,hdr_.ui_nits); metadata.MaxContentLightLevel=UINT16(metadata.MaxMasteringLuminance);
            check(chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10,sizeof metadata,&metadata),"DirectX HDR metadata");
        } else check(chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE,0,nullptr),"Clear DirectX HDR metadata");
    }
    hdr_metadata_dirty_=false;
}
void D3D12Renderer::rebuild_output() {
    if(!width_ || !height_) return;
    idle();
    if(generation_) generation_->release_context();
    for(auto& resource:backbuffers_) resource.Reset(); backbuffer_views_.Reset();
    check(swapchain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709),"Reset DirectX output color space");
    UINT support=0; ComPtr<IDXGISwapChain4> metadata_chain;
    hdr_active_=hdr_.enabled && SDL_GetBooleanProperty(SDL_GetWindowProperties(window_),SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN,false) && SUCCEEDED(swapchain_.As(&metadata_chain)) &&
        SUCCEEDED(swapchain_->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,&support)) && (support&DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT);
    const auto format=hdr_active_ ? DXGI_FORMAT_R10G10B10A2_UNORM : color_format;
    if(generation_recreate_) {
        metadata_chain.Reset();
        if(exclusive_) check(swapchain_->SetFullscreenState(FALSE,nullptr),"Leave DirectX fullscreen for frame generation");
        if(generation_) retired_generated_+=generation_->generated();
        swapchain_.Reset(); generation_.reset(); generation_kind_=UpscalerKind::off;
        ComPtr<IDXGIFactory2> factory; check(CreateDXGIFactory2(info_ ? DXGI_CREATE_FACTORY_DEBUG : 0,IID_PPV_ARGS(&factory)),"Recreate DirectX frame generation factory");
        const auto hwnd=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window_),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
        DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=width_; desc.Height=height_; desc.Format=format; desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD; desc.Flags=tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        if(generation_requested_) {
            generation_=make_native_frame_generation(upscaler_kind_,device_.Get(),queue_.Get(),shaders_.parent_path().parent_path(),streamline_.get(),generation_multiplier_); generation_kind_=upscaler_kind_;
            generation_->create_swapchain(factory.Get(),hwnd,desc,swapchain_.GetAddressOf());
        } else {
            auto presentation=presentation_factory(factory.Get(),streamline_.get());
            ComPtr<IDXGISwapChain1> chain; check(presentation->CreateSwapChainForHwnd(queue_.Get(),hwnd,&desc,nullptr,nullptr,&chain),"Recreate DirectX swapchain"); check(chain.As(&swapchain_),"Recreated DirectX swapchain interface");
        }
        check(factory->MakeWindowAssociation(hwnd,DXGI_MWA_NO_ALT_ENTER),"Recreated DXGI window association");
        if(exclusive_) check(swapchain_->SetFullscreenState(TRUE,nullptr),"Restore DirectX fullscreen");
        generation_recreate_=false;
    } else check(swapchain_->ResizeBuffers(2,width_,height_,format,tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0),"Resize DirectX output");
    check(swapchain_->SetColorSpace1(hdr_active_ ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709),"DirectX output color space");
    backbuffer_views_=heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV,UINT(backbuffers_.size()));
    for(UINT i=0;i<backbuffers_.size();++i) {
        check(swapchain_->GetBuffer(i,IID_PPV_ARGS(&backbuffers_[i])),"DirectX output backbuffer");
        auto handle=backbuffer_views_->GetCPUDescriptorHandleForHeapStart(); handle.ptr+=size_t(i)*rtv_stride_;
        device_->CreateRenderTargetView(backbuffers_[i].Get(),nullptr,handle);
    }
    if(!output_ || output_->width!=width_ || output_->height!=height_) output_=image(width_,height_,1,color_format,true);
    ui_image_.reset(); invalidate_held(); held_color_.reset(); held_depth_.reset(); last_output_=0; bloom_valid_=false;
    hdr_metadata_dirty_=true; hdr_metadata(); output_dirty_=false;
    configure_generation();
}
void D3D12Renderer::configure_generation() {
    hudless_={}; hudless_sdr_.reset(); hudless_taken_=generation_prepared_=false;
    if(!generation_) return;
    generation_->configure({width_,height_},internal_,uint32_t(hdr_active_ ? DXGI_FORMAT_R10G10B10A2_UNORM : color_format),hdr_active_ ? float(hdr_.peak_nits) : 0.f);
    for(auto& target:hudless_) target=image(width_,height_,1,hdr_active_ ? DXGI_FORMAT_R10G10B10A2_UNORM : color_format,true);
    if(hdr_active_) hudless_sdr_=image(width_,height_,1,color_format,true);
}
void D3D12Renderer::take_hudless(Slot& slot,const gx::FramePacket& packet) {
    if(!frame_generation_active() || !generation_->active() || !generation_prepared_ || hudless_taken_ || packet.black_output || !frame_scene_ || scene_phase_) return;
    const auto target=hudless_[guest_frame_&1]; if(!target) throw std::logic_error("DirectX frame generation HUD-less target is unavailable");
    const auto viewport=boundary_.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
    const uint32_t left=uint32_t(std::clamp(viewport[0],0.f,639.f)),top=uint32_t(std::clamp(viewport[1],0.f,479.f));
    const std::array<uint32_t,4> region{left,top,uint32_t(std::clamp(viewport[2],1.f,float(640-left))),uint32_t(std::clamp(viewport[3],1.f,float(480-top)))};
    if(hdr_active_) {
        blit(slot,*efb_,0,*hudless_sdr_,0,copy_uv(region,efb_->width,efb_->height),0,false);
        gx::FramePacket hdr_packet; hdr_packet.display_source=region;
        encode_hdr(slot,hdr_packet,0,nullptr,target.get(),hudless_sdr_.get()); slot.images.push_back(hudless_sdr_);
    } else blit(slot,*efb_,0,*target,0,copy_uv(region,efb_->width,efb_->height),0,false);
    transition(slot,*target,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); slot.images.push_back(target);
    generation_->tag_hudless(slot.command.Get(),{target->resource.Get(),uint32_t(target->format),uint32_t(target->states[0]),{target->width,target->height}});
    ID3D12DescriptorHeap* heaps[]{slot.descriptors.Get(),slot.sampler_heaps.back().Get()}; slot.command->SetDescriptorHeaps(2,heaps); slot.command->SetGraphicsRootSignature(root_.Get());
    hudless_taken_=true;
}
void D3D12Renderer::encode_hdr(Slot& slot,const gx::FramePacket& packet,UINT backbuffer,Image* ui,Image* destination,Image* source) {
    Image& scene=source ? *source : *output_;
    if(!hdr_pipeline_) {
        const auto vertex=shader_bytes(shaders_/"hdr.dx12.vert.cso"),pixel=shader_bytes(shaders_/"hdr.dx12.frag.cso");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{}; desc.pRootSignature=root_.Get(); desc.VS={vertex.data(),vertex.size()}; desc.PS={pixel.data(),pixel.size()};
        desc.SampleMask=UINT_MAX; desc.SampleDesc.Count=1; desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; desc.RasterizerState.DepthClipEnable=TRUE;
        desc.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        auto& blend=desc.BlendState.RenderTarget[0]; blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE; blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;
        blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD; blend.LogicOp=D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask=15;
        desc.NumRenderTargets=1; desc.RTVFormats[0]=DXGI_FORMAT_R10G10B10A2_UNORM;
        const auto result=device_->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&hdr_pipeline_)); if(FAILED(result)) stats(); check(result,"Create DirectX HDR pipeline"); ++stats_.state_objects;
    }
    struct Constants {
        int32_t origin[2]{},size[2]{},extent[2]{};
        float paper=0,peak=0,ui=0;
        uint32_t has_overlay=0;
        float bloom_origin[2]{},bloom_size[2]{};
        uint32_t has_bloom=0,reserved=0;
    } constants;
    static_assert(sizeof(Constants)==64);
    constants.size[0]=int32_t(scene.width); constants.size[1]=int32_t(scene.height); constants.extent[0]=int32_t(width_); constants.extent[1]=int32_t(height_);
    constants.paper=float(hdr_.paper_white_nits); constants.peak=float(hdr_.peak_nits); constants.ui=float(hdr_.ui_nits); constants.has_overlay=ui ? 1u : 0u;
    if(bloom_valid_ && !packet.black_output) {
        const auto& r=packet.display_source; const uint32_t x=uint32_t(uint64_t(r[0])*efb_->width/640),y=uint32_t(uint64_t(r[1])*efb_->height/480);
        const uint32_t right=std::max(x+1,uint32_t(uint64_t(r[0]+r[2])*efb_->width/640)),bottom=std::max(y+1,uint32_t(uint64_t(r[1]+r[3])*efb_->height/480));
        constants.bloom_origin[0]=x/float(bloom_region_.width); constants.bloom_origin[1]=y/float(bloom_region_.height);
        constants.bloom_size[0]=(right-x)/float(bloom_region_.width); constants.bloom_size[1]=(bottom-y)/float(bloom_region_.height); constants.has_bloom=1;
    }
    hdr_metadata();
    ID3D12DescriptorHeap* heaps[]{slot.descriptors.Get(),slot.sampler_heaps.back().Get()}; slot.command->SetDescriptorHeaps(2,heaps); slot.command->SetGraphicsRootSignature(root_.Get());
    transition(slot,scene,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); if(ui) transition(slot,*ui,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if(constants.has_bloom) { transition(slot,*bloom_up_[0],D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); slot.images.push_back(bloom_up_[0]); }
    const UINT table=descriptors(slot); srv(slot,table,scene); srv(slot,table+1,ui ? *ui : scene); srv(slot,table+2,constants.has_bloom ? *bloom_up_[0] : scene);
    const auto offset=upload(slot,&constants,sizeof constants); std::array<uint16_t,8> modes{}; modes.fill(256); const auto sampler=sampler_table(slot,modes);
    slot.command->SetGraphicsRootConstantBufferView(0,slot.upload->GetGPUVirtualAddress()+offset); slot.command->SetGraphicsRootDescriptorTable(2,gpu(slot,table)); slot.command->SetGraphicsRootDescriptorTable(3,sampler);
    D3D12_RESOURCE_BARRIER barrier{};
    D3D12_CPU_DESCRIPTOR_HANDLE target{};
    if(destination) { transition(slot,*destination,D3D12_RESOURCE_STATE_RENDER_TARGET); target=rtv(*destination); }
    else { barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition={backbuffers_[backbuffer].Get(),0,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET}; slot.command->ResourceBarrier(1,&barrier); target=backbuffer_views_->GetCPUDescriptorHandleForHeapStart(); target.ptr+=size_t(backbuffer)*rtv_stride_; }
    slot.command->OMSetRenderTargets(1,&target,FALSE,nullptr);
    const D3D12_VIEWPORT viewport{0,0,float(width_),float(height_),0,1}; const D3D12_RECT scissor{0,0,LONG(width_),LONG(height_)};
    slot.command->RSSetViewports(1,&viewport); slot.command->RSSetScissorRects(1,&scissor); slot.command->SetPipelineState(hdr_pipeline_.Get()); slot.command->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); slot.command->DrawInstanced(3,1,0,0);
    if(!destination) { std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter); slot.command->ResourceBarrier(1,&barrier); bloom_valid_=false; }
}

void D3D12Renderer::initialize(SDL_Window* window,bool validation,const std::filesystem::path& shaders,double scale,double aspect,bool exclusive) {
    if(device_) throw std::logic_error("DirectX renderer already initialized");
    scale_=device_render_scale(scale,16384,aspect); aspect_=aspect; exclusive_=exclusive;
    shaders_=shaders;
    dlss5_device_=false; dlss5_gap_=dlss5_allowed_ ? std::string{} : "DLSS 5 is off";
    if(dlss5_allowed_) dlss5_device_=probe_native_dlss5(shaders.parent_path().parent_path(),dlss5_gap_);
    {   // Loaded for every run, as on Vulkan: DLSS, Frame Generation and Ray Reconstruction then switch live, without a new device (a restart costs seconds)
        std::string reason; const auto directory=shaders.parent_path().parent_path();
        streamline_=Streamline::load(directory,true,reason,true,RendererBackend::directx12);
        if(!streamline_ && !controls_.effects.ray_reconstruction) streamline_=Streamline::load(directory,true,reason,false,RendererBackend::directx12);
        if(!streamline_ && upscaler_kind_==UpscalerKind::dlss) throw std::runtime_error("DirectX DLSS unavailable: "+reason);
    }
    window_=window;
    if(validation) {
        ComPtr<ID3D12Debug> debug; check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"DirectX 12 debug layer unavailable"); debug->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory6> factory; check(CreateDXGIFactory2(validation ? DXGI_CREATE_FACTORY_DEBUG : 0,IID_PPV_ARGS(&factory)),"Create DXGI factory");
    for(UINT index=0;;++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT found=factory->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter));
        if(found==DXGI_ERROR_NOT_FOUND) break;
        check(found,"Enumerate DirectX adapter");
        DXGI_ADAPTER_DESC1 description{}; check(adapter->GetDesc1(&description),"Adapter description");
        if(description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device_)))) break;
    }
    if(!device_) throw std::runtime_error("No hardware DirectX 12 adapter");
    {
        std::string reason; rt_supported_=NativeRtScene::supported(device_.Get(),reason);
        if((controls_.effects.ray_tracing || controls_.effects.path_tracing || controls_.effects.ray_reconstruction) && !rt_supported_) throw std::runtime_error("DirectX ray tracing unavailable: "+reason);
        effects_=effective_effects(controls_.effects,rt_supported_);
    }
    if(streamline_) {
        std::string reason; const auto luid=device_->GetAdapterLuid(); std::array<uint8_t,8> bytes{}; static_assert(sizeof luid==bytes.size()); std::memcpy(bytes.data(),&luid,sizeof luid);
        const bool dlss=upscaler_kind_==UpscalerKind::dlss;
        if(!streamline_->set_native_device(device_.Get(),reason) || !streamline_->native_dlss_supported(bytes,reason)) {
            if(dlss) throw std::runtime_error("DirectX DLSS unavailable: "+reason);
            streamline_.reset();   // no NVIDIA support on this adapter: DLSS stays off, FSR and the rest do not need it
        }
        if(streamline_) {
            if(generation_requested_ && dlss && !streamline_->native_dlss_g_supported(bytes,reason)) throw std::runtime_error("DirectX DLSS Frame Generation unavailable: "+reason);
            rr_supported_=streamline_->ray_reconstruction_loaded() && streamline_->native_ray_reconstruction_supported(bytes,reason);
            if(controls_.effects.ray_reconstruction && dlss && !rr_supported_) throw std::runtime_error("DirectX DLSS Ray Reconstruction unavailable: "+reason);
        }
    }
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model{D3D_SHADER_MODEL_6_0};
    check(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&shader_model,sizeof(shader_model)),"DirectX shader model query");
    if(shader_model.HighestShaderModel<D3D_SHADER_MODEL_6_0) throw std::runtime_error("DirectX 12 requires shader model 6.0");
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{}; check(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,&options,sizeof(options)),"DirectX options");
    if(!options.OutputMergerLogicOp) throw std::runtime_error("DirectX adapter lacks GX logic operations");
    if(validation) check(device_.As(&info_),"DirectX validation queue");
    srv_stride_=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtv_stride_=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    sampler_stride_=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device_->CreateCommandQueue(&queue,IID_PPV_ARGS(&queue_)),"Create DirectX queue");
    check(queue_->GetTimestampFrequency(&timestamp_frequency_),"DirectX timestamp frequency");
    check(device_->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_)),"Create DirectX fence");
    event_=CreateEventW(nullptr,FALSE,FALSE,nullptr); if(!event_) throw std::runtime_error("Cannot create DirectX fence event");
    BOOL allow_tearing=FALSE;
    if(SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&allow_tearing,sizeof(allow_tearing)))) tearing_=allow_tearing!=FALSE;
    const auto hwnd=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
    if(!hwnd) throw std::runtime_error("DirectX renderer requires an SDL Win32 window");
    int width=0,height=0; SDL_GetWindowSizeInPixels(window,&width,&height); width_=uint32_t(std::max(1,width)); height_=uint32_t(std::max(1,height));
    DXGI_SWAP_CHAIN_DESC1 swap{}; swap.Width=width_; swap.Height=height_; swap.Format=color_format; swap.SampleDesc.Count=1;
    swap.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; swap.BufferCount=2; swap.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap.Flags=tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if(generation_requested_) {
        generation_=make_native_frame_generation(upscaler_kind_,device_.Get(),queue_.Get(),shaders.parent_path().parent_path(),streamline_.get(),generation_multiplier_); generation_kind_=upscaler_kind_;
        generation_->create_swapchain(factory.Get(),hwnd,swap,swapchain_.GetAddressOf());
    } else {
        auto presentation=presentation_factory(factory.Get(),streamline_.get());
        ComPtr<IDXGISwapChain1> chain; check(presentation->CreateSwapChainForHwnd(queue_.Get(),hwnd,&swap,nullptr,nullptr,&chain),"Create DirectX swapchain"); check(chain.As(&swapchain_),"DirectX swapchain interface");
    }
    check(factory->MakeWindowAssociation(hwnd,DXGI_MWA_NO_ALT_ENTER),"DXGI window association");
    if(exclusive_) check(swapchain_->SetFullscreenState(TRUE,nullptr),"DirectX exclusive fullscreen");
    for(UINT i=0;i<backbuffers_.size();++i) check(swapchain_->GetBuffer(i,IID_PPV_ARGS(&backbuffers_[i])),"DirectX backbuffer");
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,texture_registers,0,0,0};
    D3D12_ROOT_PARAMETER parameters[4]{};
    for(UINT i=0;i<2;++i) { parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[i].Descriptor.ShaderRegister=i; parameters[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL; }
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[2].DescriptorTable={1,&ranges[0]};
    ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,8,0,0,0}; parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[3].DescriptorTable={1,&ranges[1]}; parameters[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{}; signature.NumParameters=UINT(std::size(parameters)); signature.pParameters=parameters; signature.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob,error;
    const auto serialized=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);
    if(FAILED(serialized) && error) throw std::runtime_error(std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()));
    check(serialized,"Serialize DirectX root signature"); check(device_->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root_)),"Create DirectX root signature");
    vertex_shader_=shader_bytes(shaders/"smoke.dx12.vert.cso"); pixel_shader_=shader_bytes(shaders/"smoke.dx12.frag.cso");
    dxbc_vertex_=shader_bytes(shaders/"smoke.dx11.vert.cso"); dxbc_motion_vertex_=shader_bytes(shaders/"smoke.motion.dx11.vert.cso");   // same source and registers; the DXBC pair for the specialized pixel shaders
    tev_=std::make_unique<TevShaders>(shaders,shaders.parent_path(),TevShaders::Finish{},std::getenv("UC_DX_SPEC_SYNC")!=nullptr);
    blit_vertex_=shader_bytes(shaders/"blit.dx12.vert.cso"); blit_pixel_=shader_bytes(shaders/"blit.dx12.frag.cso");
    D3D12_QUERY_HEAP_DESC query{}; query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP; query.Count=UINT(slot_count*2);
    check(device_->CreateQueryHeap(&query,IID_PPV_ARGS(&queries_)),"Create timestamp queries");
    for(auto& slot:slots_) {
        check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&slot.allocator)),"Create command allocator");
        check(device_->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot.allocator.Get(),nullptr,IID_PPV_ARGS(&slot.command)),"Create command list");
        check(slot.command->Close(),"Close initial command list"); slot.timing=buffer(16,D3D12_HEAP_TYPE_READBACK);
    }
    targets(); configure_upscaler(); samplers(); initialized_=true;
    if(rt_supported_) rt_effects_warm_=std::async(std::launch::async,[device=device_.Get(),directory=shaders_]() -> std::unique_ptr<NativeRtEffects> {
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
        try { return std::make_unique<NativeRtEffects>(device,directory); } catch(const std::exception&) { return nullptr; }   // the first path-traced frame builds it and reports the error
    });
    if(dlss5_device_) {
        dlss5_context_={device_.Get(),shaders_,shaders_.parent_path().parent_path(),[this] { idle(); },{}};
        dlss5_pass_.configure(dlss5_context_);
    }
}
void D3D12Renderer::samplers() {
    samplers_=heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,513);
    const D3D12_TEXTURE_ADDRESS_MODE address[]{D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_WRAP,D3D12_TEXTURE_ADDRESS_MODE_MIRROR};
    for(UINT index=0;index<513;++index) {
        const UINT mode=index>=257 ? index-257 : index;
        if(mode<256 && ((mode&3)>2 || ((mode>>2)&3)>2 || ((mode>>5)&3)>2)) continue;
        D3D12_SAMPLER_DESC desc{}; desc.AddressU=address[mode<256 ? mode&3 : 0]; desc.AddressV=address[mode<256 ? (mode>>2)&3 : 0]; desc.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.MaxAnisotropy=std::clamp(controls_.anisotropy,1u,16u); desc.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        const UINT mip=(mode>>5)&3; desc.MaxLOD=mode==256 || mip ? D3D12_FLOAT32_MAX : 0;
        desc.Filter=mode==256 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_ENCODE_BASIC_FILTER(mode&128 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,mode&16 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,mip==2 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,D3D12_FILTER_REDUCTION_TYPE_STANDARD);
        if(mode<256 && (mode&128) && desc.MaxAnisotropy>1) desc.Filter=D3D12_FILTER_ANISOTROPIC;
        if(index>=257 && internal_.width && efb_) desc.MipLODBias=mip_bias(internal_,{efb_->width,efb_->height});
        auto handle=samplers_->GetCPUDescriptorHandleForHeapStart(); handle.ptr+=size_t(index)*sampler_stride_; device_->CreateSampler(&desc,handle);
    }
}
void D3D12Renderer::targets() {
    const auto extent=manual_render_extent(scale_,aspect_); const auto height=uint32_t(std::llround(960*scale_));
    efb_=image(extent.width,height,1,color_format,true,false,true);
    depth_=image(extent.width,height,1,DXGI_FORMAT_D32_FLOAT,false,true);
    output_=image(width_,height_,1,color_format,true); efb_initialized_=false; last_output_=last_efb_=0;
}
void D3D12Renderer::collect() {
    if(!fence_) return;
    const auto value=fence_->GetCompletedValue();
    if(value==UINT64_MAX) check(device_->GetDeviceRemovedReason(),"DirectX device removed");
    std::vector<std::pair<uint64_t,uint64_t>> retired;
    for(auto& slot:slots_) if(slot.fence && slot.fence<=value) {
        if(slot.serial) retired.emplace_back(slot.fence,slot.serial);
        void* mapped=nullptr; D3D12_RANGE range{0,16}; check(slot.timing->Map(0,&range,&mapped),"Read GPU timing");
        uint64_t timestamps[2]; std::memcpy(timestamps,mapped,sizeof timestamps); D3D12_RANGE empty{}; slot.timing->Unmap(0,&empty);
        if(timestamps[1]>=timestamps[0] && timestamp_frequency_) stats_.gpu_busy_ns+=uint64_t(double(timestamps[1]-timestamps[0])*1e9/double(timestamp_frequency_));
        slot.fence=slot.serial=0; slot.images.clear(); slot.resources.clear(); slot.rt_frames.clear(); slot.rt_work.clear(); slot.dlss5_work.clear();
    }
    std::sort(retired.begin(),retired.end()); for(const auto& entry:retired) completed_.push_back(entry.second);
    // Expired CPU owners can remain briefly: every submitted GPU use has its own slot reference.
    if(value>=swept_fence_ && value-swept_fence_>=64) {
        swept_fence_=value;
        std::erase_if(textures_,[](const auto& item) { return item.second->source.expired(); });
        for(auto at=copies_.begin();at!=copies_.end();) {
            if(at->second->source.expired() && at->second.use_count()==1 && at->second->written<=value) {
                auto retired=std::move(at->second); at=copies_.erase(at); recycle_copy(std::move(retired));
            } else ++at;
        }
        std::erase_if(aliases_,[](const auto& item) { return item.second.requested.expired(); });
        std::erase_if(meshes_,[&](const auto& item) { if(!item.second.source.expired()) return false; rt_scene_.invalidate_mesh(item.second.id); return true; });
        std::erase_if(latest_copies_,[](const auto& item) { return item.second.expired(); });
    }
}
bool D3D12Renderer::wait(uint64_t value,uint64_t timeout_ns) {
    if(!value || fence_->GetCompletedValue()>=value) { collect(); return true; }
    if(!timeout_ns) return false;
    check(fence_->SetEventOnCompletion(value,event_),"Wait for DirectX fence");
    const DWORD timeout=timeout_ns==UINT64_MAX ? INFINITE : DWORD(std::min<uint64_t>((timeout_ns+999999)/1000000,INFINITE-1));
    const auto result=WaitForSingleObject(event_,timeout);
    if(result==WAIT_FAILED) throw std::runtime_error("DirectX fence wait failed");
    collect();
    // Earlier timed waits can signal this event too; only the requested fence proves completion.
    if(result==WAIT_OBJECT_0 && fence_->GetCompletedValue()<value) return wait(value,timeout_ns);
    return fence_->GetCompletedValue()>=value;
}
void D3D12Renderer::idle() {
    if(!queue_ || !fence_) return;
    if(generation_) generation_->drain();
    const auto value=next_fence_++; check(queue_->Signal(fence_.Get(),value),"Signal DirectX idle"); wait(value,UINT64_MAX);
}
Slot* D3D12Renderer::begin(size_t bytes,UINT descriptor_count) {
    collect();
    const auto free=std::find_if(slots_.begin(),slots_.end(),[](const auto& slot) { return slot.fence==0; });
    if(free==slots_.end()) return nullptr;
    Slot& slot=*free;
    if(bytes>512ull*1024*1024 || descriptor_count>1000000) throw std::invalid_argument("DirectX packet exceeds bounded upload capacity");
    if(bytes>slot.capacity) {
        if(slot.upload) slot.upload->Unmap(0,nullptr);
        slot.capacity=aligned(std::max<size_t>(bytes,65536),65536); slot.upload=buffer(slot.capacity);
        void* mapped=nullptr; D3D12_RANGE empty{}; check(slot.upload->Map(0,&empty,&mapped),"Map packet upload"); slot.mapped=static_cast<std::byte*>(mapped);
    }
    if(descriptor_count>slot.descriptor_capacity) {
        slot.descriptor_capacity=std::max(256u,descriptor_count); slot.descriptors=heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,slot.descriptor_capacity,true);
        slot.source_descriptors=heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,slot.descriptor_capacity);
    }
    slot.used=0; slot.descriptor_used=0; slot.sampler_used=0; slot.sampler_tables.clear();
    slot.source_used=0; slot.palettes.clear(); slot.banks.clear(); slot.texture_tables.clear();
    if(slot.sampler_heaps.empty()) slot.sampler_heaps.push_back(heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,2048,true));
    else slot.sampler_heaps.resize(1);
    check(slot.allocator->Reset(),"Reset DirectX allocator"); check(slot.command->Reset(slot.allocator.Get(),nullptr),"Reset DirectX commands");
    ID3D12DescriptorHeap* heaps[]{slot.descriptors.Get(),slot.sampler_heaps.back().Get()}; slot.command->SetDescriptorHeaps(2,heaps); slot.command->SetGraphicsRootSignature(root_.Get());
    slot.command->EndQuery(queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,UINT((&slot-slots_.data())*2));
    return &slot;
}
uint64_t D3D12Renderer::submit(Slot& slot,uint64_t serial) {
    const auto query=UINT((&slot-slots_.data())*2);
    slot.command->EndQuery(queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,query+1);
    slot.command->ResolveQueryData(queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,query,2,slot.timing.Get(),0);
    check(slot.command->Close(),"Close DirectX commands");
    ID3D12CommandList* lists[]{slot.command.Get()}; queue_->ExecuteCommandLists(1,lists);
    slot.fence=next_fence_++; slot.serial=serial;
    check(queue_->Signal(fence_.Get(),slot.fence),"Signal DirectX submission"); return slot.fence;
}

std::shared_ptr<Image> D3D12Renderer::upload_texture(Slot& slot,const gx::TexturePixels* pixels) {
    const gx::TextureLevel fallback{1,1,{255,255,255,255}};
    if(pixels) pixels->wait();
    if(pixels && (pixels->levels.empty() || pixels->levels.size()>15 || pixels->gpu_only || pixels->channels!=gx::TextureChannels::rgba)) throw std::invalid_argument("Invalid owned DirectX texture");
    const auto& first=pixels ? pixels->levels.front() : fallback;
    const UINT count=pixels ? UINT(pixels->levels.size()) : 1;
    auto result=image(first.width,first.height,count,pixels && pixels->bc7 ? DXGI_FORMAT_BC7_UNORM : color_format,false);
    const auto desc=result->resource->GetDesc(); std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count); std::vector<UINT> rows(count); std::vector<UINT64> row_bytes(count); UINT64 total=0;
    device_->GetCopyableFootprints(&desc,0,count,0,layouts.data(),rows.data(),row_bytes.data(),&total);
    auto staging=buffer(size_t(total)); void* mapped=nullptr; D3D12_RANGE empty{}; check(staging->Map(0,&empty,&mapped),"Map texture upload");
    for(UINT mip=0;mip<count;++mip) {
        const auto& level=pixels ? pixels->levels[mip] : fallback;
        if(level.width!=std::max(1u,first.width>>mip) || level.height!=std::max(1u,first.height>>mip) || level.rgba.size()!=size_t(row_bytes[mip])*rows[mip]) { staging->Unmap(0,nullptr); throw std::invalid_argument("Invalid owned DirectX mip chain"); }
        for(UINT row=0;row<rows[mip];++row) std::memcpy(static_cast<std::byte*>(mapped)+layouts[mip].Offset+size_t(row)*layouts[mip].Footprint.RowPitch,level.rgba.data()+size_t(row)*row_bytes[mip],size_t(row_bytes[mip]));
        D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource=result->resource.Get(); destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; destination.SubresourceIndex=mip;
        D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource=staging.Get(); source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; source.PlacedFootprint=layouts[mip];
        slot.command->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
    }
    staging->Unmap(0,nullptr); slot.resources.push_back(staging);
    transition(slot,*result,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); return result;
}
std::shared_ptr<Image> D3D12Renderer::texture(Slot& slot,const std::shared_ptr<const gx::TexturePixels>& pixels) {
    if(!pixels) { if(!white_) white_=upload_texture(slot,nullptr); return white_; }
    if(pixels->gpu_only) {
        if(const auto copy=copies_.find(pixels.get());copy!=copies_.end() && copy->second->source.lock()==pixels) return copy->second;
        if(const auto alias=aliases_.find(pixels.get());alias!=aliases_.end() && alias->second.requested.lock()==pixels) {
            if(alias->second.image) return alias->second.image;
            // A previous-frame copy has no history on its first frame.
            if(!black_) { gx::TexturePixels black; black.levels.push_back({1,1,{0,0,0,0}}); black_=upload_texture(slot,&black); }
            return black_;
        }
        if(lost_.contains(pixels.get())) {
            if(!black_) { gx::TexturePixels black; black.levels.push_back({1,1,{0,0,0,0}}); black_=upload_texture(slot,&black); }
            return black_;
        }
        throw std::invalid_argument("DirectX EFB texture sampled before its copy");
    }
    auto& cached=textures_[pixels.get()];
    if(!cached || cached->source.lock()!=pixels) { cached=upload_texture(slot,pixels.get()); cached->source=pixels; }
    return cached;
}
std::shared_ptr<Image> D3D12Renderer::copy_texture(Slot& slot,const gx::TextureCopy& request) {
    const auto& target=request.target; if(!target || !target->gpu_only) throw std::invalid_argument("Invalid DirectX EFB copy target"); target->wait();
    if(target->levels.empty() || target->levels.size()>15 || target->bc7 || unsigned(target->channels)>2) throw std::invalid_argument("Invalid DirectX EFB copy format");
    const auto& first=target->levels.front();
    if(!first.width || !first.height || first.width>1024 || first.height>1024) throw std::invalid_argument("Invalid GX EFB copy extent");
    for(size_t i=0;i<target->levels.size();++i) if(target->levels[i].width!=std::max(1u,first.width>>i) || target->levels[i].height!=std::max(1u,first.height>>i) || !target->levels[i].rgba.empty()) throw std::invalid_argument("Invalid GX EFB copy mip chain");
    const uint32_t width=std::max(1u,uint32_t((uint64_t(first.width)*efb_->width+320)/640)),height=std::max(1u,uint32_t((uint64_t(first.height)*efb_->height+240)/480));
    const UINT mips=std::min(UINT(target->levels.size()),UINT(std::bit_width(std::max(width,height))));
    const auto matches=[&](const auto& candidate) { return candidate->width==width && candidate->height==height && candidate->mips==mips && candidate->format==color_format && candidate->channels==target->channels; };
    std::shared_ptr<Image> copy;
    if(const auto recycled=std::find_if(recycled_copies_.begin(),recycled_copies_.end(),matches);recycled!=recycled_copies_.end()) {
        copy=std::move(*recycled); recycled_copies_.erase(recycled); recycled_copy_bytes_-=copy->allocation_size;
    } else {
        // A targeted miss check avoids waiting for the periodic sweep to refill a small, busy copy class.
        const auto completed=fence_->GetCompletedValue();
        for(auto at=copies_.begin();at!=copies_.end();++at) {
            if(matches(at->second) && at->second->source.expired() && at->second.use_count()==1 && at->second->written<=completed) {
                copy=std::move(at->second); copies_.erase(at); forget_copy_address(*copy); break;
            }
        }
    }
    if(!copy) copy=image(width,height,mips,color_format,true);
    copy->source=target; copy->channels=target->channels; copy->guest_address=target->guest_address;
    const auto source=scene_phase_ && raster_color_ ? raster_color_ : efb_;
    blit(slot,*source,0,*copy,0,copy_uv(request.source,source->width,source->height),uint32_t(target->channels));
    for(UINT mip=1;mip<copy->mips;++mip) blit(slot,*copy,mip-1,*copy,mip,{0,0,1,1},uint32_t(target->channels));
    transition(slot,*copy,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); slot.images.push_back(copy);
    // Queue order makes this version sampleable by later packets; retirement still waits for its fence.
    copy->written=next_fence_;
    copies_[target.get()]=copy; aliases_.erase(target.get()); if(target->guest_address) latest_copies_[target->guest_address]=copy;
    return copy;
}
void D3D12Renderer::forget_copy_address(const Image& image) {
    if(const auto latest=latest_copies_.find(image.guest_address);latest!=latest_copies_.end() && latest->second.lock().get()==&image) latest_copies_.erase(latest);
}
void D3D12Renderer::recycle_copy(std::shared_ptr<Image> copy) {
    forget_copy_address(*copy);
    const double factor=efb_ ? std::max(efb_->width/640.0,efb_->height/480.0) : 1;
    const auto budget=uint64_t(std::min(double(uint64_t(512)<<20),double(uint64_t(64)<<20)*factor*factor));
    if(recycled_copies_.size()>=32 || recycled_copy_bytes_>=budget || copy->allocation_size>budget-recycled_copy_bytes_) return;
    copy->source.reset(); copy->written=0; copy->guest_address=0;
    recycled_copies_.push_back(std::move(copy)); recycled_copy_bytes_+=recycled_copies_.back()->allocation_size;
}

ID3D12PipelineState* D3D12Renderer::pipeline(const gx::DrawPacket& draw,bool alpha_pass,bool scene,bool surface_pass,const TevWords* tev) {
    auto blend=draw.blend; const auto cull=draw.state->cull;
    if(blend.mode>3 || blend.source>7 || blend.destination>7 || blend.logic>15 || cull>3) throw std::invalid_argument("Invalid DirectX GX state");
    if(alpha_pass) blend={0,1,0,3,false,true}; else if(draw.uniforms->pixel[2]&256) blend.alpha_write=false;
    if(surface_pass) blend={0,1,0,3,false,false};
    if(blend.mode!=1) { blend.source=1; blend.destination=0; } if(blend.mode!=2) blend.logic=3;
    auto depth=draw.uniforms->pixel[1]&31;
    if(alpha_pass) { if((depth&17)==17) depth=5; else depth&=~16u; }
    if(surface_pass) depth&=~16u;
    bool lines=false;
    switch(draw.primitive) { case 0x80: case 0x90: case 0x98: case 0xA0: break; case 0xA8: case 0xB0: lines=true; break; default: throw std::invalid_argument("Unsupported DirectX GX primitive"); }
    const bool material=scene && rt_active();
    const uint64_t key=blend.mode|(blend.source<<2)|(blend.destination<<5)|(blend.logic<<8)|(uint32_t(blend.color_write)<<12)|(uint32_t(blend.alpha_write)<<13)|(uint32_t(cull)<<14)|(depth<<16)|(uint32_t(lines)<<21)|(uint32_t(scene)<<22)|(uint32_t(alpha_pass)<<23)|(uint32_t(surface_pass)<<24)|(uint32_t(material)<<25);
    ID3D12PipelineState* uber=nullptr;
    if(const auto found=pipelines_.find(key);found!=pipelines_.end()) uber=found->second.Get();
    TevShaders::Bytecode spec_code; SpecKey spec_key{};
    if(tev && tev_ && blend.mode!=2 && !surface_pass) {   // logic operations keep the ubershader
        if(const auto* entry=tev_->get(scene ? 1 : 0,*tev); entry && entry->code) {
            spec_key={key,entry->code.get()};
            if(const auto found=spec_pipelines_.find(spec_key);found!=spec_pipelines_.end()) { if(found->second) return found->second.Get(); }
            else if(spec_pending_.insert(spec_key).second) spec_code=entry->code;
        }
    }
    if(uber && !spec_code) return uber;
    static const std::array<D3D12_INPUT_ELEMENT_DESC,14> input=[] {   // static: the specialized PSOs are created on other threads from a copy of the description
        std::array<D3D12_INPUT_ELEMENT_DESC,14> layout{{
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,UINT(offsetof(gx::GpuMeshVertex,position)),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,UINT(offsetof(gx::GpuMeshVertex,color)),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"BLENDINDICES",0,DXGI_FORMAT_R32_UINT,0,UINT(offsetof(gx::GpuMeshVertex,row)),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"BLENDINDICES",1,DXGI_FORMAT_R32G32_UINT,0,UINT(offsetof(gx::GpuMeshVertex,texture_matrix)),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"COLOR",1,DXGI_FORMAT_R8G8B8A8_UNORM,0,UINT(offsetof(gx::GpuMeshVertex,color1)),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"NORMAL",1,DXGI_FORMAT_R32G32B32_FLOAT,0,UINT(offsetof(gx::GpuMeshVertex,object_normal)),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}}};
        for(UINT i=0;i<8;++i) layout[6+i]={"TEXCOORD",3+i,DXGI_FORMAT_R32G32_FLOAT,0,UINT(offsetof(gx::GpuMeshVertex,texcoords)+i*8),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
        return layout;
    }();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{}; desc.pRootSignature=root_.Get(); desc.VS={vertex_shader_.data(),vertex_shader_.size()}; desc.PS={pixel_shader_.data(),pixel_shader_.size()};
    if(scene) { desc.VS={motion_vertex_.data(),motion_vertex_.size()}; desc.PS={motion_pixel_.data(),motion_pixel_.size()}; }
    if(blend.mode==2) {
        auto& shader=scene ? motion_logic_pixel_ : logic_pixel_;
        if(shader.empty()) shader=shader_bytes(shaders_/(scene ? "smoke.motion.logic.color.dx12.frag.cso" : "smoke.logic.dx12.frag.cso"));
        desc.PS={shader.data(),shader.size()};
    }
    desc.InputLayout={input.data(),UINT(input.size())}; desc.SampleMask=UINT_MAX; desc.SampleDesc.Count=1;
    desc.PrimitiveTopologyType=lines ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; desc.RasterizerState.CullMode=cull==1 ? D3D12_CULL_MODE_BACK : cull==2 ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_NONE;
    desc.RasterizerState.FrontCounterClockwise=FALSE; desc.RasterizerState.DepthClipEnable=TRUE;
    desc.NumRenderTargets=1; desc.RTVFormats[0]=blend.mode==2 ? DXGI_FORMAT_R8G8B8A8_UINT : color_format; desc.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    desc.DepthStencilState.DepthEnable=(depth&1)!=0; desc.DepthStencilState.DepthWriteMask=(depth&16) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC(((depth>>1)&7)+1);
    const D3D12_BLEND sources[]{D3D12_BLEND_ZERO,D3D12_BLEND_ONE,D3D12_BLEND_DEST_COLOR,D3D12_BLEND_INV_DEST_COLOR,D3D12_BLEND_SRC_ALPHA,D3D12_BLEND_INV_SRC_ALPHA,D3D12_BLEND_DEST_ALPHA,D3D12_BLEND_INV_DEST_ALPHA};
    const D3D12_BLEND destinations[]{D3D12_BLEND_ZERO,D3D12_BLEND_ONE,D3D12_BLEND_SRC_COLOR,D3D12_BLEND_INV_SRC_COLOR,D3D12_BLEND_SRC_ALPHA,D3D12_BLEND_INV_SRC_ALPHA,D3D12_BLEND_DEST_ALPHA,D3D12_BLEND_INV_DEST_ALPHA};
    const auto alpha_factor=[](D3D12_BLEND factor) { switch(factor) { case D3D12_BLEND_SRC_COLOR: return D3D12_BLEND_SRC_ALPHA; case D3D12_BLEND_INV_SRC_COLOR: return D3D12_BLEND_INV_SRC_ALPHA; case D3D12_BLEND_DEST_COLOR: return D3D12_BLEND_DEST_ALPHA; case D3D12_BLEND_INV_DEST_COLOR: return D3D12_BLEND_INV_DEST_ALPHA; default: return factor; } };
    const D3D12_LOGIC_OP logic[]{D3D12_LOGIC_OP_CLEAR,D3D12_LOGIC_OP_AND,D3D12_LOGIC_OP_AND_REVERSE,D3D12_LOGIC_OP_COPY,D3D12_LOGIC_OP_AND_INVERTED,D3D12_LOGIC_OP_NOOP,D3D12_LOGIC_OP_XOR,D3D12_LOGIC_OP_OR,D3D12_LOGIC_OP_NOR,D3D12_LOGIC_OP_EQUIV,D3D12_LOGIC_OP_INVERT,D3D12_LOGIC_OP_OR_REVERSE,D3D12_LOGIC_OP_COPY_INVERTED,D3D12_LOGIC_OP_OR_INVERTED,D3D12_LOGIC_OP_NAND,D3D12_LOGIC_OP_SET};
    auto& target=desc.BlendState.RenderTarget[0]; target.BlendEnable=blend.mode==1 || blend.mode==3; target.LogicOpEnable=blend.mode==2;
    target.SrcBlend=blend.mode==3 ? D3D12_BLEND_ONE : sources[blend.source]; target.DestBlend=blend.mode==3 ? D3D12_BLEND_ONE : destinations[blend.destination];
    target.SrcBlendAlpha=alpha_factor(target.SrcBlend); target.DestBlendAlpha=alpha_factor(target.DestBlend);
    target.BlendOp=target.BlendOpAlpha=blend.mode==3 ? D3D12_BLEND_OP_REV_SUBTRACT : D3D12_BLEND_OP_ADD; target.LogicOp=logic[blend.logic];
    target.RenderTargetWriteMask=(blend.color_write ? 7 : 0)|(blend.alpha_write ? 8 : 0);
    if(scene && blend.mode!=2) {
        desc.NumRenderTargets=material ? 5 : 3; desc.RTVFormats[1]=DXGI_FORMAT_R16G16B16A16_FLOAT; desc.RTVFormats[2]=DXGI_FORMAT_R16G16B16A16_UNORM; desc.BlendState.IndependentBlendEnable=TRUE;
        if(material) { desc.RTVFormats[3]=DXGI_FORMAT_R32_UINT; desc.RTVFormats[4]=color_format; }
        for(UINT i=1;i<desc.NumRenderTargets;++i) {
            auto& extra=desc.BlendState.RenderTarget[i]; extra.SrcBlend=extra.SrcBlendAlpha=D3D12_BLEND_ONE; extra.DestBlend=extra.DestBlendAlpha=D3D12_BLEND_ZERO;
            extra.BlendOp=extra.BlendOpAlpha=D3D12_BLEND_OP_ADD; extra.LogicOp=D3D12_LOGIC_OP_NOOP;
            extra.RenderTargetWriteMask=!alpha_pass && (draw.uniforms->pixel[1]&17)==17 ? (i==1 ? 3 : 15) : 0;
            if(i>=3 && draw.blend.mode==2) extra.RenderTargetWriteMask=0;
        }
    }
    if(spec_code) {
        auto specialized=desc; const auto& vertex=scene ? dxbc_motion_vertex_ : dxbc_vertex_;
        specialized.VS={vertex.data(),vertex.size()}; specialized.PS={spec_code->data(),spec_code->size()};
        tev_->post([this,specialized,spec_key,spec_code,device=device_.Get()] {
            ComPtr<ID3D12PipelineState> state;
            if(FAILED(device->CreateGraphicsPipelineState(&specialized,IID_PPV_ARGS(&state)))) state.Reset();
            std::lock_guard lock(spec_mutex_); spec_done_.push_back({spec_key,std::move(state)});
        });
        if(tev_->blocking()) { drain_spec(); if(const auto found=spec_pipelines_.find(spec_key);found!=spec_pipelines_.end() && found->second) return found->second.Get(); }
    }
    if(uber) return uber;
    ComPtr<ID3D12PipelineState> state;
    const auto began=std::chrono::steady_clock::now();
    const auto created=device_->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&state));
    if(FAILED(created)) stats();
    check(created,"Create GX DirectX PSO");
    // The Vulkan "[pipeline] ... compiled on the render thread" line: this call is a stall of the frame (the ubershader PSO of a state combination seen for the first time).
    if(const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count(); ms>=2) std::fprintf(stderr,"[pipeline] new ubershader PSO compiled on the render thread: %.1f ms\n",ms);
    ++stats_.state_objects; return pipelines_.emplace(key,std::move(state)).first->second.Get();
}

void D3D12Renderer::blit(Slot& slot,Image& source,UINT source_mip,Image& destination,UINT destination_mip,const std::array<float,4>& uv,uint32_t channels,bool linear,RenderExtent area) {
    if(!blit_pipeline_) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{}; desc.pRootSignature=root_.Get(); desc.VS={blit_vertex_.data(),blit_vertex_.size()}; desc.PS={blit_pixel_.data(),blit_pixel_.size()};
        desc.SampleMask=UINT_MAX; desc.SampleDesc.Count=1; desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; desc.RasterizerState.DepthClipEnable=TRUE;
        desc.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        auto& target=desc.BlendState.RenderTarget[0]; target.SrcBlend=target.SrcBlendAlpha=D3D12_BLEND_ONE; target.DestBlend=target.DestBlendAlpha=D3D12_BLEND_ZERO;
        target.BlendOp=target.BlendOpAlpha=D3D12_BLEND_OP_ADD; target.LogicOp=D3D12_LOGIC_OP_NOOP; target.RenderTargetWriteMask=15;
        desc.NumRenderTargets=1; desc.RTVFormats[0]=color_format;
        check(device_->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&blit_pipeline_)),"Create DirectX blit PSO"); ++stats_.state_objects;
    }
    transition(slot,source,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,source_mip); transition(slot,destination,D3D12_RESOURCE_STATE_RENDER_TARGET,destination_mip);
    const auto target=rtv(destination,destination_mip); slot.command->OMSetRenderTargets(1,&target,FALSE,nullptr);
    const uint32_t width=area.width ? area.width : std::max(1u,destination.width>>destination_mip),height=area.height ? area.height : std::max(1u,destination.height>>destination_mip);
    const D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1}; const D3D12_RECT scissor{0,0,LONG(width),LONG(height)};
    slot.command->RSSetViewports(1,&viewport); slot.command->RSSetScissorRects(1,&scissor);
    struct BlitConstants { std::array<float,4> uv; uint32_t channels,pad[3]; } constants{uv,channels,{}};
    const auto offset=upload(slot,&constants,sizeof constants); slot.command->SetGraphicsRootConstantBufferView(0,slot.upload->GetGPUVirtualAddress()+offset);
    const UINT table=descriptors(slot); srv(slot,table,source,source_mip,1);
    std::array<uint16_t,8> modes{}; modes.fill(linear ? 256 : 0); const auto sampler=sampler_table(slot,modes);
    slot.command->SetGraphicsRootDescriptorTable(2,gpu(slot,table)); slot.command->SetGraphicsRootDescriptorTable(3,sampler);
    slot.command->SetPipelineState(blit_pipeline_.Get()); slot.command->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); slot.command->DrawInstanced(3,1,0,0);
}

void D3D12Renderer::draw(Slot& slot,const gx::DrawPacket& draw,const gx::FramePacket& packet,Image& target,Image& depth_target,bool overlay_frame,bool scene) {
    const auto vertices=draw.vertex_view(); const auto indices=draw.index_view();
    if(indices.empty() || vertices.empty() || draw.state->cull==3 || (draw.reticle && controls_.crosshair_percent==0)) return;
    if(draw.material.stages>16) throw std::invalid_argument("DirectX TEV stage count exceeds 16");
    const bool indexed=draw.state->descriptors[0]!=gx::Mode::none;
    const bool retarget=draw.wake && controls_.aim_history && controls_.wake_seconds>0;
    const size_t vertex_bytes=vertices.size()*sizeof(gx::GpuMeshVertex),index_bytes=indices.size()*sizeof(uint32_t);
    D3D12_VERTEX_BUFFER_VIEW vertex_view{}; D3D12_INDEX_BUFFER_VIEW index_view{};
    ID3D12Resource* mesh_resource=slot.upload.Get(); uint64_t mesh_id=0;
    if(draw.mesh && !retarget) {
        auto& mesh=meshes_[draw.mesh.get()];
        if(!mesh.buffer || mesh.source.lock()!=draw.mesh) {
            for(auto index:indices) if(index>=vertices.size()) throw std::invalid_argument("DirectX mesh index out of bounds");
            const auto rows=gx::used_rows(vertices);
            gx::validate_draw_rows(*draw.transforms,indexed,rows,draw.callsite);
            if(mesh.id) rt_scene_.invalidate_mesh(mesh.id);
            mesh.id=next_mesh_id_++;
            mesh.source=draw.mesh; mesh.rows=rows; mesh.buffer=buffer(aligned(vertex_bytes)+index_bytes);
            void* data=nullptr; D3D12_RANGE empty{}; check(mesh.buffer->Map(0,&empty,&data),"Map DirectX mesh");
            auto* target=static_cast<gx::GpuMeshVertex*>(data); for(size_t i=0;i<vertices.size();++i) target[i]=gx::mesh_vertex(vertices[i]);
            std::memcpy(static_cast<std::byte*>(data)+aligned(vertex_bytes),indices.data(),index_bytes); mesh.buffer->Unmap(0,nullptr);
            mesh.vertices={mesh.buffer->GetGPUVirtualAddress(),UINT(vertex_bytes),sizeof(gx::GpuMeshVertex)};
            mesh.indices={mesh.buffer->GetGPUVirtualAddress()+aligned(vertex_bytes),UINT(index_bytes),DXGI_FORMAT_R32_UINT};
        } else gx::validate_draw_rows(*draw.transforms,indexed,mesh.rows,draw.callsite);
        vertex_view=mesh.vertices; index_view=mesh.indices; slot.resources.push_back(mesh.buffer);
        mesh_resource=mesh.buffer.Get(); mesh_id=mesh.id;
    } else {
        for(auto index:indices) if(index>=vertices.size()) throw std::invalid_argument("DirectX mesh index out of bounds");
        gx::validate_draw_rows(*draw.transforms,indexed,gx::used_rows(vertices),draw.callsite);
        const auto offset=upload(slot,nullptr,vertex_bytes,16); auto* target=reinterpret_cast<gx::GpuMeshVertex*>(slot.mapped+offset);
        for(size_t i=0;i<vertices.size();++i) target[i]=gx::mesh_vertex(vertices[i]);
        if(retarget && gx::retarget_wake(draw,target,vertices.size(),controls_.wake_seconds,controls_.aim_history)) ++stats_.late_aim_draws;
        vertex_view={slot.upload->GetGPUVirtualAddress()+offset,UINT(vertex_bytes),sizeof(gx::GpuMeshVertex)};
        const auto index_offset=upload(slot,indices.data(),index_bytes,4); index_view={slot.upload->GetGPUVirtualAddress()+index_offset,UINT(index_bytes),DXGI_FORMAT_R32_UINT};
    }
    struct Constants { gx::DrawUniform draw; std::array<uint32_t,68> tev{}; } constants;
    static_assert(sizeof(Constants)==2112);
    constants.draw=*draw.uniforms; constants.tev[0]=draw.material.stages;
    for(size_t i=0;i<4;++i) constants.tev[1]|=uint32_t(draw.material.swaps[i])<<(8*i);
    for(size_t i=0;i<draw.material.stages;++i) { const auto& stage=draw.material.stage[i]; constants.tev[2+i*4]=stage.color; constants.tev[3+i*4]=stage.alpha; constants.tev[4+i*4]=stage.order; constants.tev[5+i*4]=stage.konst; }
    const auto constant_offset=upload(slot,&constants,sizeof constants);
    TevWords tev_words; std::copy_n(constants.tev.begin(),66,tev_words.begin()); tev_words[66]=0;
    auto palette=slot.palettes.find(&*draw.transforms);
    if(palette==slot.palettes.end()) {
        gx::PaletteBlock block{}; gx::fill_palette(block,*draw.transforms);
        const StructuredView view{upload(slot,&block,sizeof block,16),slot.source_used++}; structured(slot,view.descriptor,view.offset,129);
        palette=slot.palettes.emplace(&*draw.transforms,view).first;
    }
    StructuredView palette_view=palette->second;
    UINT motion_descriptor=palette_view.descriptor;
    size_t motion_offset=0;
    const bool vectors=scene && (draw.uniforms->pixel[1]&17)==17;
    if(scene) {
        if(effects_.contact_shadows || effects_.shadows!=EffectLevel::off) scene_lights_.add(*draw.uniforms);
        if(vectors && draw.transforms->projection[3][3]==0 && !frame_projection_) {
            frame_projection_=draw.transforms->projection;
            if(effects_.shadows!=EffectLevel::off && dominant_light_ && dominant_light_->kind==gx::LightKind::directional)
                shadow_fit_=fit_shadow(*frame_projection_,*dominant_light_,30.f,shadow_preset(effects_.shadows).resolution);
        }
        if(draw.transforms->projection[3][3]==0) scene_projection_=draw.transforms->projection;
        const auto key=tracker_->key(draw);
        if(vectors) {
            MotionBlock block;
            const auto viewport=draw.state->viewport.value_or(std::array<float,6>{0,0,640,480,0,1});
            const auto original=efb_viewport(viewport,target.width,target.height);
            if(const auto* previous=tracker_->previous(key)) {
                block.kind=1; block.previous=previous->transforms->projection; block.palette=1;
                std::array<float,2> shift{}; native_viewport(original,block.previous,shift);
                std::array<gx::PaletteBlock,2> blocks{}; gx::fill_palette(blocks[0],*draw.transforms); gx::fill_palette(blocks[1],*previous->transforms);
                palette_view={upload(slot,blocks.data(),sizeof blocks,16),slot.source_used++}; structured(slot,palette_view.descriptor,palette_view.offset,258);
            } else { block.kind=2; block.previous=native_camera_motion(original,tracker_->clip_to_prev_clip()); }
            upload(slot,nullptr,((slot.used+79)/80*80)-slot.used,1);
            motion_offset=upload(slot,&block,sizeof block,1); motion_descriptor=slot.source_used++;
            D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.ViewDimension=D3D12_SRV_DIMENSION_BUFFER; view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Buffer.FirstElement=motion_offset/80; view.Buffer.NumElements=1; view.Buffer.StructureByteStride=80;
            device_->CreateShaderResourceView(slot.upload.Get(),&view,source_cpu(slot,motion_descriptor));
        }
        tracker_->record(key,draw.transforms.block(),(draw.uniforms->pixel[1]&17)==17);
    }
    std::array<uint16_t,8> modes{};
    std::array<std::shared_ptr<Image>,8> sampled;
    TextureTableKey table_key{};
    for(UINT map=0;map<8;++map) {
        sampled[map]=texture(slot,map ? draw.texture_maps[map-1] : draw.texture); transition(slot,*sampled[map],D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        table_key[map]=reinterpret_cast<uintptr_t>(sampled[map].get());
        const auto wrap=draw.texture_wrap[map];
        if((wrap&3)>2 || ((wrap>>2)&3)>2 || ((wrap>>5)&3)>2) throw std::invalid_argument("Invalid DirectX texture wrap");
        modes[map]=uint16_t(wrap+(scene && reconstruction() ? 257 : 0));
    }
    const auto sampler=sampler_table(slot,modes);
    if(bound_.samplers.ptr!=sampler.ptr) { slot.command->SetGraphicsRootDescriptorTable(3,sampler); bound_.samplers=sampler; }
    DrawPush push; push.projection=draw.transforms->projection; push.current_row=draw.transforms->current; push.indexed=draw.state->descriptors[0]!=gx::Mode::none;
    if(scene && reconstruction()) { push.jitter[0]=jitter_[0]; push.jitter[1]=jitter_[1]; }
    if(vectors) push.motion=0;
    push.effects=controls_.per_pixel_lighting ? 1u : 0u;
    if(scene && rt_active()) {
        push.effects|=kEffectRtMaterial;
        if(vectors && draw.blend.mode!=2) {
            const auto& uniforms=*draw.uniforms; const auto lighting=gx::channel_lighting(uniforms.channels[0]),second=gx::channel_lighting(uniforms.channels[1]);
            const auto flags=rt_record_flags(lighting,light_model_valid(draw.material,lighting,second));
            const float reflectivity=rt_reflectivity({uniforms.materials[1][0],uniforms.materials[1][1],uniforms.materials[1][2]},second.lit && second.attenuation==1);
            push.record=uint32_t(rt_records_.size());
            rt_records_.push_back({uniforms.materials[0],uniforms.ambient[0],lighting.mask,push.record*8,rt_reflect_pack(reflectivity,flags),0.f});
            rt_lights_.insert(rt_lights_.end(),uniforms.lights.begin(),uniforms.lights.end());
            if(flags&kRtModelValid) push.effects|=kEffectRtModelValid;
        }
        const bool triangles=draw.primitive==0x80 || draw.primitive==0x90 || draw.primitive==0x98 || draw.primitive==0xA0;
        if(draw.mesh && vectors && triangles && draw.transforms->projection[3][3]==0) {
            NativeRtGeometry native; auto& geometry=native.geometry;
            geometry.vertices=vertex_view.BufferLocation; geometry.indices=index_view.BufferLocation;
            geometry.vertex_count=uint32_t(vertices.size()); geometry.index_count=uint32_t(indices.size());
            geometry.current=draw.transforms->current; geometry.indexed=push.indexed; geometry.palettes=slot.upload->GetGPUVirtualAddress()+palette_view.offset;
            const auto alpha=draw.uniforms->pixel[0]; geometry.opaque=((alpha>>16)&7)==7 && ((alpha>>19)&7)==7;
            geometry.record=push.record; geometry.skinned=push.indexed!=0;
            if(!geometry.skinned) { geometry.mesh_id=mesh_id; for(UINT row=0;row<3;++row) geometry.transform[row]=draw.transforms->position_rows[geometry.current+row]; }
            native.vertex_buffer=native.index_buffer=mesh_resource; native.palette_buffer=slot.upload.Get();
            if(mesh_resource==slot.upload.Get()) {
                // Hit shading can outlive this packet slot; retain an immutable index copy.
                auto retained=buffer(index_bytes); void* mapped=nullptr; D3D12_RANGE empty{};
                check(retained->Map(0,&empty,&mapped),"Map ray tracing indices"); std::memcpy(mapped,indices.data(),index_bytes); retained->Unmap(0,nullptr);
                geometry.indices=retained->GetGPUVirtualAddress(); native.index_buffer=retained.Get(); slot.resources.push_back(retained);
            }
            rt_geometry_.push_back(native);
        }
    }
    StructuredView bank_view=palette->second;
    if(gx::has_texture_matrix_index(*draw.state)) {
        if(!draw.texture_bank) throw std::invalid_argument("Indexed texture matrix has no bank");
        auto bank=slot.banks.find(draw.texture_bank.get());
        if(bank==slot.banks.end()) {
            const StructuredView view{upload(slot,draw.texture_bank.get(),sizeof(gx::TextureBank),16),slot.source_used++}; structured(slot,view.descriptor,view.offset,378);
            bank=slot.banks.emplace(draw.texture_bank.get(),view).first;
        }
        bank_view=bank->second; push.bank=0;
    }
    table_key[8]=palette_view.offset; table_key[9]=bank_view.offset; table_key[10]=vectors ? motion_offset+1 : 0;
    auto cached_table=slot.texture_tables.find(table_key);
    if(cached_table==slot.texture_tables.end()) {
        const UINT first=descriptors(slot),count=texture_registers;
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE,texture_registers> sources{};
        for(UINT map=0;map<8;++map) { sources[map]=sampled_view(*sampled[map]); slot.images.push_back(sampled[map]); }
        sources[8]=source_cpu(slot,palette_view.descriptor); sources[9]=source_cpu(slot,bank_view.descriptor); sources[10]=source_cpu(slot,motion_descriptor);
        const auto destination=cpu(slot,first);
        device_->CopyDescriptors(1,&destination,&count,count,sources.data(),nullptr,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        cached_table=slot.texture_tables.emplace(table_key,first).first;
    }
    const UINT table=cached_table->second;
    const auto v=draw.state->viewport.value_or(std::array<float,6>{0,0,640,480,0,1});
    if(!std::all_of(v.begin(),v.end(),[](float value) { return std::isfinite(value); }) || v[2]<=0 || v[3]<=0) throw std::invalid_argument("Invalid DirectX viewport");
    auto area=efb_viewport(v,target.width,target.height);
    if(overlay_frame) {
        const auto& source=packet.display_source; const float sx=target.width/float(source[2]),sy=target.height/float(source[3]);
        area={(v[0]-1.f/12-source[0])*sx,(v[1]-1.f/12-source[1])*sy,v[2]*sx,v[3]*sy};
    }
    D3D12_VIEWPORT viewport{area.x,area.y,area.width,area.height,std::clamp(v[4],0.f,1.f),std::clamp(v[5],0.f,1.f)};
    D3D12_RECT scissor{0,0,LONG(target.width),LONG(target.height)};
    if(draw.state->scissor && !overlay_frame) {
        const auto& s=*draw.state->scissor;
        scissor.left=LONG(std::clamp<int64_t>(int64_t(s[0])*target.width/640,0,target.width)); scissor.top=LONG(std::clamp<int64_t>(int64_t(s[1])*target.height/480,0,target.height));
        scissor.right=LONG(std::clamp<int64_t>((int64_t(s[0])+s[2])*target.width/640,scissor.left,target.width)); scissor.bottom=LONG(std::clamp<int64_t>((int64_t(s[1])+s[3])*target.height/480,scissor.top,target.height));
    }
    if(draw.reticle && controls_.crosshair_percent!=100) if(const auto center=gx::draw_center_ndc(draw)) gx::scale_viewport_about(*center,controls_.crosshair_percent/100.f,viewport.TopLeftX,viewport.TopLeftY,viewport.Width,viewport.Height);
    if(draw.late_aim && controls_.aim && !retarget) {
        const auto latest=controls_.aim(); const auto& from=draw.aim_exact ? draw.aim_ref : packet.aim_consumed;
        push.shift[0]=(latest[0]-from[0])*gx::kAimToNdcX*area.width/viewport.Width; push.shift[1]=(latest[1]-from[1])*gx::kAimToNdcY*area.height/viewport.Height;
        if(push.shift[0]!=0 || push.shift[1]!=0) { scissor={0,0,LONG(target.width),LONG(target.height)}; ++stats_.late_aim_draws; }
    }
    const auto adjusted=native_viewport({viewport.TopLeftX,viewport.TopLeftY,viewport.Width,viewport.Height},push.projection,push.shift);
    viewport.TopLeftX=adjusted.viewport.x; viewport.TopLeftY=adjusted.viewport.y; viewport.Width=adjusted.viewport.width; viewport.Height=adjusted.viewport.height;
    scissor.left=std::max(scissor.left,LONG(adjusted.clip[0])); scissor.top=std::max(scissor.top,LONG(adjusted.clip[1]));
    scissor.right=std::max(scissor.left,std::min(scissor.right,LONG(adjusted.clip[2]))); scissor.bottom=std::max(scissor.top,std::min(scissor.bottom,LONG(adjusted.clip[3])));
    push.viewport[0]=viewport.TopLeftX; push.viewport[1]=viewport.TopLeftY; push.viewport[2]=viewport.Width; push.viewport[3]=viewport.Height;
    const auto push_offset=upload(slot,&push,sizeof push);
    transition(slot,target,D3D12_RESOURCE_STATE_RENDER_TARGET); transition(slot,depth_target,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const auto depth=depth_target.views->GetCPUDescriptorHandleForHeapStart();
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE,5> colors{draw.blend.mode==2 ? logic_rtv(target) : rtv(target)};
    const UINT target_count=scene ? (rt_active() ? 5 : 3) : 1;
    if(scene) { transition(slot,*scene_motion_,D3D12_RESOURCE_STATE_RENDER_TARGET); transition(slot,*scene_gbuffer_,D3D12_RESOURCE_STATE_RENDER_TARGET); colors[1]=rtv(*scene_motion_); colors[2]=rtv(*scene_gbuffer_); }
    if(scene && rt_active()) { transition(slot,*scene_material_,D3D12_RESOURCE_STATE_RENDER_TARGET); transition(slot,*scene_albedo_,D3D12_RESOURCE_STATE_RENDER_TARGET); colors[3]=rtv(*scene_material_); colors[4]=rtv(*scene_albedo_); }
    const bool logic=draw.blend.mode==2;
    bind_targets(slot,!logic ? target_count : 1,colors,depth);
    if(!bound_.rasterizer_set || std::memcmp(&bound_.viewport,&viewport,sizeof viewport)) { slot.command->RSSetViewports(1,&viewport); bound_.viewport=viewport; }
    if(!bound_.rasterizer_set || std::memcmp(&bound_.scissor,&scissor,sizeof scissor)) { slot.command->RSSetScissorRects(1,&scissor); bound_.scissor=scissor; }
    bound_.rasterizer_set=true;
    slot.command->SetGraphicsRootConstantBufferView(0,slot.upload->GetGPUVirtualAddress()+constant_offset); slot.command->SetGraphicsRootConstantBufferView(1,slot.upload->GetGPUVirtualAddress()+push_offset);
    const auto textures=gpu(slot,table);
    if(bound_.textures.ptr!=textures.ptr) { slot.command->SetGraphicsRootDescriptorTable(2,textures); bound_.textures=textures; }
    if(std::memcmp(&bound_.vertex,&vertex_view,sizeof vertex_view)) { slot.command->IASetVertexBuffers(0,1,&vertex_view); bound_.vertex=vertex_view; }
    if(std::memcmp(&bound_.index,&index_view,sizeof index_view)) { slot.command->IASetIndexBuffer(&index_view); bound_.index=index_view; }
    const auto topology=draw.primitive==0xA8 || draw.primitive==0xB0 ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    if(bound_.topology!=topology) { slot.command->IASetPrimitiveTopology(topology); bound_.topology=topology; }
    const auto depth_bits=draw.uniforms->pixel[1]&31;
    if(scene && logic && (depth_bits&17)==17) {
        const auto surface=pipeline(draw,false,true,true),color=pipeline(draw,false,true);
        const UINT primitive=draw.primitive==0xA8 || draw.primitive==0xB0 ? 2 : 3;
        // Interleave each primitive so overlap observes exactly the depth left by its predecessor.
        for(UINT first=0;first<UINT(indices.size());first+=primitive) {
            const UINT count=std::min(primitive,UINT(indices.size())-first);
            colors[0]=rtv(target); bind_targets(slot,target_count,colors,depth);
            bind_pso(slot,surface); slot.command->DrawIndexedInstanced(count,1,first,0,0);
            colors[0]=logic_rtv(target); bind_targets(slot,1,colors,depth);
            bind_pso(slot,color); slot.command->DrawIndexedInstanced(count,1,first,0,0);
        }
    } else { bind_pso(slot,pipeline(draw,false,scene,false,&tev_words)); slot.command->DrawIndexedInstanced(UINT(indices.size()),1,0,0,0); }
    if((draw.uniforms->pixel[2]&256) && draw.blend.alpha_write && !((depth_bits&1) && ((depth_bits>>1)&7)==0)) {
        if(draw.blend.mode==2) { colors[0]=rtv(target); bind_targets(slot,target_count,colors,depth); }
        push.alpha_pass=1; const auto alpha_offset=upload(slot,&push,sizeof push);
        slot.command->SetGraphicsRootConstantBufferView(1,slot.upload->GetGPUVirtualAddress()+alpha_offset); tev_words[66]=1; bind_pso(slot,pipeline(draw,true,scene,false,&tev_words)); slot.command->DrawIndexedInstanced(UINT(indices.size()),1,0,0,0);
    }
    if(scene && shadow_fit_) { shadow_draw(slot,draw,vertex_view,index_view,UINT(indices.size())); bound_={}; }   // the shadow pass binds its own state
}
void D3D12Renderer::shadow_draw(Slot& slot,const gx::DrawPacket& draw,const D3D12_VERTEX_BUFFER_VIEW& vertices,const D3D12_INDEX_BUFFER_VIEW& indices,UINT count) {
    const auto alpha=draw.uniforms->pixel[0];
    const bool triangles=draw.primitive==0x80 || draw.primitive==0x90 || draw.primitive==0x98 || draw.primitive==0xA0;
    if(!draw.mesh || !count || draw.transforms->projection[3][3]!=0 || (draw.uniforms->pixel[1]&17)!=17 || !triangles || ((alpha>>16)&7)!=7 || ((alpha>>19)&7)!=7) return;
    if(!shadow_pipeline_) {
        const auto shader=shader_bytes(shaders_/"smoke.shadow.dx12.vert.cso");
        D3D12_INPUT_ELEMENT_DESC input[14]{
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,UINT(offsetof(gx::GpuMeshVertex,position)),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,UINT(offsetof(gx::GpuMeshVertex,color)),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"BLENDINDICES",0,DXGI_FORMAT_R32_UINT,0,UINT(offsetof(gx::GpuMeshVertex,row)),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"BLENDINDICES",1,DXGI_FORMAT_R32G32_UINT,0,UINT(offsetof(gx::GpuMeshVertex,texture_matrix)),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"COLOR",1,DXGI_FORMAT_R8G8B8A8_UNORM,0,UINT(offsetof(gx::GpuMeshVertex,color1)),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"NORMAL",1,DXGI_FORMAT_R32G32B32_FLOAT,0,UINT(offsetof(gx::GpuMeshVertex,object_normal)),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
        for(UINT i=0;i<8;++i) input[6+i]={"TEXCOORD",3+i,DXGI_FORMAT_R32G32_FLOAT,0,UINT(offsetof(gx::GpuMeshVertex,texcoords)+i*8),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{}; desc.pRootSignature=root_.Get(); desc.VS={shader.data(),shader.size()}; desc.InputLayout={input,UINT(std::size(input))};
        desc.SampleMask=UINT_MAX; desc.SampleDesc.Count=1; desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; desc.DSVFormat=DXGI_FORMAT_D32_FLOAT;
        desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; desc.RasterizerState.DepthClipEnable=TRUE;
        desc.RasterizerState.DepthBias=2; desc.RasterizerState.SlopeScaledDepthBias=2.f;
        desc.DepthStencilState.DepthEnable=TRUE; desc.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL; desc.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
        const auto result=device_->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&shadow_pipeline_)); if(FAILED(result)) stats(); check(result,"Create DirectX shadow pipeline"); ++stats_.state_objects;
    }
    const auto resolution=shadow_preset(effects_.shadows).resolution;
    if(!shadow_map_ || shadow_map_->width!=resolution) { shadow_map_=image(resolution,resolution,1,DXGI_FORMAT_D32_FLOAT,false,true); shadow_cleared_=false; }
    transition(slot,*shadow_map_,D3D12_RESOURCE_STATE_DEPTH_WRITE); slot.images.push_back(shadow_map_);
    const auto target=shadow_map_->views->GetCPUDescriptorHandleForHeapStart();
    if(!shadow_cleared_) slot.command->ClearDepthStencilView(target,D3D12_CLEAR_FLAG_DEPTH,1.f,0,0,nullptr);
    shadow_cleared_=true;
    slot.command->OMSetRenderTargets(0,nullptr,FALSE,&target);
    const D3D12_VIEWPORT viewport{0,0,float(resolution),float(resolution),0,1}; const D3D12_RECT scissor{0,0,LONG(resolution),LONG(resolution)};
    slot.command->RSSetViewports(1,&viewport); slot.command->RSSetScissorRects(1,&scissor);
    DrawPush push; push.projection=shadow_fit_->view_to_light; push.current_row=draw.transforms->current; push.indexed=draw.state->descriptors[0]!=gx::Mode::none; push.viewport[2]=push.viewport[3]=float(resolution);
    const auto offset=upload(slot,&push,sizeof push); slot.command->SetGraphicsRootConstantBufferView(1,slot.upload->GetGPUVirtualAddress()+offset);
    slot.command->IASetVertexBuffers(0,1,&vertices); slot.command->IASetIndexBuffer(&indices); slot.command->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    slot.command->SetPipelineState(shadow_pipeline_.Get()); slot.command->DrawIndexedInstanced(count,1,0,0,0);
}
void D3D12Renderer::clear_raster(Slot& slot,const gx::FramePacket& packet,const std::array<uint32_t,4>* source) {
    transition(slot,*raster_color_,D3D12_RESOURCE_STATE_RENDER_TARGET); transition(slot,*raster_depth_,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_RECT rectangle{};
    if(source) {
        const auto& r=*source; const LONG x=LONG(uint64_t(r[0])*raster_color_->width/640),y=LONG(uint64_t(r[1])*raster_color_->height/480);
        rectangle={x,y,std::max(x+1,LONG(uint64_t(r[0]+r[2])*raster_color_->width/640)),std::max(y+1,LONG(uint64_t(r[1]+r[3])*raster_color_->height/480))};
    }
    float color[4]; for(UINT i=0;i<4;++i) color[i]=packet.clear_color[i]/255.f;
    slot.command->ClearRenderTargetView(rtv(*raster_color_),color,source ? 1 : 0,source ? &rectangle : nullptr);
    slot.command->ClearDepthStencilView(raster_depth_->views->GetCPUDescriptorHandleForHeapStart(),D3D12_CLEAR_FLAG_DEPTH,packet.clear_depth/float(0xFFFFFF),0,source ? 1 : 0,source ? &rectangle : nullptr);
    if(motion_cleared_) {
        const float zero[4]{};
        for(const auto& image:{scene_motion_,scene_gbuffer_}) { transition(slot,*image,D3D12_RESOURCE_STATE_RENDER_TARGET); slot.command->ClearRenderTargetView(rtv(*image),zero,source ? 1 : 0,source ? &rectangle : nullptr); }
    }
    slot.images.push_back(raster_color_); slot.images.push_back(raster_depth_);
}
void D3D12Renderer::clear(Slot& slot,const gx::FramePacket& packet,const D3D12_RECT* region) {
    transition(slot,*efb_,D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(slot,*depth_,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    float color[4]; for(size_t i=0;i<4;++i) color[i]=packet.clear_color[i]/255.f;
    slot.command->ClearRenderTargetView(rtv(*efb_),color,region ? 1 : 0,region);
    slot.command->ClearDepthStencilView(depth_->views->GetCPUDescriptorHandleForHeapStart(),D3D12_CLEAR_FLAG_DEPTH,packet.clear_depth/float(0xFFFFFF),0,region ? 1 : 0,region);
    if(wants_scene() && scene_phase_ && motion_cleared_) {
        const float zero[4]{};
        for(const auto& image:{scene_motion_,scene_gbuffer_}) { transition(slot,*image,D3D12_RESOURCE_STATE_RENDER_TARGET); slot.command->ClearRenderTargetView(rtv(*image),zero,region ? 1 : 0,region); }
    }
}
bool D3D12Renderer::render(const gx::FramePacket& packet,bool display,uint64_t submission_serial) {
    if(!initialized_) throw std::logic_error("DirectX renderer is not initialized");
    if(pending_scale_) { const auto scale=*pending_scale_; pending_scale_.reset(); set_render_scale(scale); }
    if(pending_controls_ && (frame_start_ || (pending_controls_->scene_targets==controls_.scene_targets && pending_controls_->effects==controls_.effects))) {
        auto controls=std::move(*pending_controls_); pending_controls_.reset(); set_controls(controls);
    }
    if(pending_dlss5_) { const auto settings=*pending_dlss5_; pending_dlss5_.reset(); set_dlss5(settings); }
    if(upscaler_dirty_) {
        idle(); upscaler_.reset(); reset_scene(); scene_phase_=false; frame_start_=true; scene_restore_copy_.reset(); configure_upscaler(); samplers(); upscaler_dirty_=false;
    }
    if(!width_ || !height_) return false;
    const uint64_t serial=submission_serial ? submission_serial : packet.serial;
    if(!serial || serial<=last_serial_) throw std::invalid_argument("DirectX submission serial must increase");
    if(packet.clear_depth>0xFFFFFF) throw std::invalid_argument("Invalid DirectX EFB clear depth");
    for(const auto& prompt:packet.prompts) {
        for(float value:prompt.efb_corners) if(!std::isfinite(value)) throw std::invalid_argument("Nonfinite DirectX prompt geometry");
        for(float value:prompt.alpha) if(!std::isfinite(value) || value<0 || value>1) throw std::invalid_argument("Invalid DirectX prompt alpha");
        for(const auto& tint:prompt.tint) for(float value:tint) if(!std::isfinite(value) || value<0 || value>1) throw std::invalid_argument("Invalid DirectX prompt tint");
        for(float value:prompt.clip) if(!std::isfinite(value)) throw std::invalid_argument("Nonfinite DirectX prompt clip");
        if(prompt.clip[2]<prompt.clip[0] || prompt.clip[3]<prompt.clip[1]) throw std::invalid_argument("Invalid DirectX prompt clip");
    }
    const auto valid_region=[](const std::array<uint32_t,4>& r) { return r[2] && r[3] && r[0]<640 && r[1]<480 && r[2]<=640-r[0] && r[3]<=480-r[1]; };
    if((packet.present || packet.clear_only) && !valid_region(packet.display_source)) throw std::invalid_argument("Invalid DirectX display copy rectangle");
    if(packet.texture_copy && !valid_region(packet.texture_copy->source)) throw std::invalid_argument("Invalid DirectX EFB copy rectangle");
    if(output_dirty_) rebuild_output();
    const bool layer_active=reticle_layer_ && !hdr_active_ && !generation_;
    const bool overlay_frame=packet.overlay_only;
    if(overlay_frame && (!packet.present || packet.texture_copy || packet.clear_after_copy || packet.clear_only)) throw std::invalid_argument("Invalid DirectX held-frame overlay packet");
    if(overlay_frame && (!display || !reticle_layer_ready())) return false;
    const bool hold=layer_active && !overlay_frame && packet.present && display && !packet.black_output;
    const auto start=Clock::now();
    drain_spec();
    size_t bytes=16384;
    for(const auto& draw:packet.draws) {
        const size_t geometry=draw.vertex_view().size()*sizeof(gx::GpuMeshVertex)+draw.index_view().size()*sizeof(uint32_t);
        if(geometry>256ull*1024*1024 || bytes>512ull*1024*1024-geometry) throw std::invalid_argument("DirectX packet geometry exceeds capacity");
        bytes+=geometry+(wants_scene() ? 32768 : 16384);
    }
    if(packet.draws.size()>80000) throw std::invalid_argument("DirectX packet has too many draws");
    auto* available=begin(bytes,UINT((packet.draws.size()+64)*texture_registers)); if(!available) return false; Slot& slot=*available;
    struct RecordingGuard { bool& flag; explicit RecordingGuard(bool& value):flag(value) { flag=true; } ~RecordingGuard() { flag=false; } } recording(recording_);
    if(!overlay_frame) {
        if(layer_active) for(const auto& draw_packet:packet.draws) if(draw_packet.layer) {
            frame_reticle_.push_back(draw_packet);
            auto& retained=frame_reticle_.back();
            if(retained.late_aim && !retained.aim_exact) { retained.aim_ref=packet.aim_consumed; retained.aim_exact=true; }
        }
        if(frame_start_) {
            dlss5_scene_ready_=false;
            scene_phase_=boundary_.frame_had_scene(); frame_start_=false;
            frame_scene_=scene_phase_; hudless_taken_=generation_prepared_=false;
            if(generation_) generation_->wait_for_inputs();
            rt_geometry_.clear(); rt_records_.clear(); rt_lights_.clear(); rt_work_.reset();
            if(rt_active()) {
                if(!rt_scene_.initialized()) rt_scene_.initialize(device_.Get(),shaders_);
                rt_scene_.begin_frame(guest_frame_);
            }
            if(wants_scene()) { if(!tracker_) tracker_=std::make_unique<gx::MotionTracker>(); tracker_->begin_frame(); motion_cleared_=false; scene_lights_.clear(); frame_projection_.reset(); shadow_fit_.reset(); shadow_cleared_=shadow_valid_=false; }
            if(streamline_ && upscaler_kind_==UpscalerKind::dlss) streamline_->begin_frame(uint32_t(guest_frame_));
            if(generation_) generation_->begin_frame(frame_generation_active() && frame_scene_ && !packet.black_output,guest_frame_,reset_next_scene_ || packet.black_output);
            if(streamline_ && generation_kind_==UpscalerKind::dlss && generation_) {
                streamline_->marker(Streamline::Marker::simulation_start); streamline_->marker(Streamline::Marker::simulation_end); streamline_->marker(Streamline::Marker::render_submit_start);
            }
            jitter_=reconstruction() ? jitter_offset(upscale_frame_,jitter_phase_count(internal_.width,efb_->width)) : std::array<float,2>{};
        }
        const auto cut=boundary_.on_packet(gx::summarize(packet));
        const auto scene_draws=scene_phase_ ? cut.value_or(packet.draws.size()) : 0;
        const bool bloom_here=scene_phase_ && (cut.has_value() || packet.present);
        if(scene_phase_ && (scene_draws || packet.present)) scene_restore_copy_.reset();
        if(!efb_initialized_) { clear(slot,packet,nullptr); efb_initialized_=true; }
        if(wants_scene() && scene_phase_) {
            scene_targets(slot);
            if(raster_color_ && !raster_initialized_) { clear_raster(slot,packet,nullptr); raster_initialized_=true; }
            if(raster_color_ && scene_clear_.pending) {
                gx::FramePacket clear_packet; clear_packet.clear_color=scene_clear_.color; clear_packet.clear_depth=scene_clear_.depth;
                clear_raster(slot,clear_packet,&scene_clear_.source); scene_clear_.pending=false;
            }
        }
        if(bloom_here && cut) record_bloom(slot,packet.black_output);
        if(frame_scene_ && !scene_phase_ && !packet.texture_copy && !packet.draws.empty()) take_hudless(slot,packet);
        size_t draw_index=0; bound_={};
        for(const auto& draw_packet:packet.draws) {
            const bool scene=wants_scene() && draw_index<scene_draws;
            if(!gx::bloom_composite(draw_packet) && !(layer_active && draw_packet.layer)) draw(slot,draw_packet,packet,scene && raster_color_ ? *raster_color_ : *efb_,scene && raster_depth_ ? *raster_depth_ : *depth_,false,scene);
            ++draw_index;
        }
        record_rt_geometry(slot);
        if(bloom_here && !cut) record_bloom(slot,packet.black_output);
        if(packet.present) take_hudless(slot,packet);
    }
    if(packet.present && generation_ && (!display || packet.black_output || !generation_prepared_ || !hudless_taken_)) generation_->begin_frame(false,guest_frame_,true);
    std::shared_ptr<Image> copy;
    if(packet.texture_copy) {
        copy=copy_texture(slot,*packet.texture_copy);
        if(scene_phase_) {
            if(!packet.clear_after_copy) scene_restore_copy_=packet.texture_copy;
            else scene_restore_copy_.reset();
        }
    }
    const bool copied=packet.present && display;
    const bool present=copied && !hold;
    if(copied) {
        const auto& r=packet.display_source;
        if(!overlay_frame && !packet.black_output) {
            const std::array<int32_t,2> origin{int32_t(uint64_t(r[0])*efb_->width/640),int32_t(uint64_t(r[1])*efb_->height/480)};
            const RenderExtent size{uint32_t(std::max<int64_t>(1,int64_t(uint64_t(r[0]+r[2])*efb_->width/640)-origin[0])),uint32_t(std::max<int64_t>(1,int64_t(uint64_t(r[1]+r[3])*efb_->height/480)-origin[1]))};
            record_dlss5(slot,Dlss5Hook::present,efb_,origin,size,{0,0},false);
        }
        if(overlay_frame) {
            transition(slot,*held_color_,D3D12_RESOURCE_STATE_COPY_SOURCE); transition(slot,*output_,D3D12_RESOURCE_STATE_COPY_DEST);
            slot.command->CopyResource(output_->resource.Get(),held_color_->resource.Get());
            slot.images.push_back(held_color_); slot.images.push_back(held_depth_);
            slot.command->ClearDepthStencilView(held_depth_->views->GetCPUDescriptorHandleForHeapStart(),D3D12_CLEAR_FLAG_DEPTH,1.f,0,0,nullptr);
            bound_={};
            for(const auto& draw_packet:packet.draws) if(draw_packet.layer) draw(slot,draw_packet,packet,*output_,*held_depth_,true);
        } else if(packet.black_output) { transition(slot,*output_,D3D12_RESOURCE_STATE_RENDER_TARGET); const float black[4]{}; slot.command->ClearRenderTargetView(rtv(*output_),black,0,nullptr); }
        else blit(slot,*efb_,0,*output_,0,copy_uv(r,efb_->width,efb_->height),0,false);
        if(hold) {
            if(!held_color_) { held_color_=image(width_,height_,1,color_format,true); held_depth_=image(width_,height_,1,DXGI_FORMAT_D32_FLOAT,false,true); }
            transition(slot,*output_,D3D12_RESOURCE_STATE_COPY_SOURCE); transition(slot,*held_color_,D3D12_RESOURCE_STATE_COPY_DEST);
            slot.command->CopyResource(held_color_->resource.Get(),output_->resource.Get()); slot.images.push_back(held_color_);
            held_valid_=true; held_presented_=false; held_source_=r; held_aim_=packet.aim_consumed;
            held_draws_=std::move(frame_reticle_); held_prompts_=packet.prompts;
        } else {
            const bool has_ui=menu_build_ || (!packet.black_output && !packet.prompts.empty());
            const auto backbuffer=swapchain_->GetCurrentBackBufferIndex();
            if(hdr_active_) {
                if(has_ui) {
                    if(!ui_image_) ui_image_=image(width_,height_,1,color_format,true);
                    transition(slot,*ui_image_,D3D12_RESOURCE_STATE_RENDER_TARGET);
                    const float transparent[4]{}; slot.command->ClearRenderTargetView(rtv(*ui_image_),transparent,0,nullptr);
                    overlay(slot,packet,*ui_image_); slot.images.push_back(ui_image_);
                }
                encode_hdr(slot,packet,backbuffer,has_ui ? ui_image_.get() : nullptr);
            } else {
                if(has_ui) overlay(slot,packet,*output_);
                transition(slot,*output_,D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition={backbuffers_[backbuffer].Get(),0,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST}; slot.command->ResourceBarrier(1,&barrier);
                slot.command->CopyResource(backbuffers_[backbuffer].Get(),output_->resource.Get()); std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter); slot.command->ResourceBarrier(1,&barrier);
            }
            if(packet.black_output) invalidate_held();
        }
    }
    if(!overlay_frame && packet.clear_after_copy && (packet.present || packet.texture_copy || packet.clear_only)) {
        const auto& r=packet.texture_copy ? packet.texture_copy->source : packet.display_source;
        const LONG left=LONG(uint64_t(r[0])*efb_->width/640),top=LONG(uint64_t(r[1])*efb_->height/480);
        const D3D12_RECT rectangle{left,top,std::max(left+1,LONG(uint64_t(r[0]+r[2])*efb_->width/640)),std::max(top+1,LONG(uint64_t(r[1]+r[3])*efb_->height/480))};
        if(scene_phase_ && raster_color_) clear_raster(slot,packet,&r); else clear(slot,packet,&rectangle);
        if(packet.present && raster_color_) scene_clear_={true,packet.display_source,packet.clear_color,packet.clear_depth};
    }
    const auto written=submit(slot,serial); if(copy) copy->written=written;
    if(packet.present && !overlay_frame && streamline_ && generation_kind_==UpscalerKind::dlss && generation_) streamline_->marker(Streamline::Marker::render_submit_end);
    last_serial_=serial; last_efb_=serial;
    if(packet.present && !overlay_frame) { frame_start_=true; frame_reticle_.clear(); dlss5_scene_ready_=false; ++guest_frame_; }
    if(copied) last_output_=serial;
    stats_.submit_ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
    if(present) {
        if(generation_) generation_->before_present();
        if(streamline_ && generation_kind_==UpscalerKind::dlss && generation_) streamline_->marker(Streamline::Marker::present_start);
        const auto before=Clock::now(); check(swapchain_->Present(0,tearing_ && !exclusive_ ? DXGI_PRESENT_ALLOW_TEARING : 0),"Present DirectX frame");
        if(generation_) generation_->after_present();
        if(streamline_ && generation_kind_==UpscalerKind::dlss && generation_) streamline_->marker(Streamline::Marker::present_end);
        stats_.present_ms=std::chrono::duration<double,std::milli>(Clock::now()-before).count(); ++stats_.presentations; last_output_=serial;        if(overlay_frame) held_presented_=true;
    }
    return true;
}
void D3D12Renderer::invalidate_held() {
    held_valid_=held_presented_=false; held_draws_.clear(); held_prompts_.clear(); frame_reticle_.clear();
}
void D3D12Renderer::set_reticle_layer(bool on) {
    if(reticle_layer_==on) return;
    reticle_layer_=on; invalidate_held();
}
bool D3D12Renderer::reticle_layer_ready() const {
    return initialized_ && reticle_layer_ && !hdr_active_ && !generation_ && held_valid_ && held_color_ && width_ && height_ && held_color_->width==width_ && held_color_->height==height_;
}
bool D3D12Renderer::render_overlay(uint64_t serial) {
    if(!reticle_layer_ready()) return false;
    collect();
    // Leave a free command slot for guest packets when overlays run faster than guest ticks.
    if(std::count_if(slots_.begin(),slots_.end(),[](const auto& slot) { return slot.fence==0; })<2) return false;
    gx::FramePacket packet; packet.serial=serial; packet.present=true; packet.overlay_only=true; packet.clear_after_copy=false;
    packet.display_source=held_source_; packet.aim_consumed=held_aim_; packet.prompts=held_prompts_;
    for(const auto& draw:held_draws_) packet.draws.push_back(draw);
    return render(packet,true,serial);
}
void D3D12Renderer::resize(uint32_t width,uint32_t height) {
    if(width==width_ && height==height_) return;
    idle(); width_=width; height_=height; output_dirty_=true; invalidate_held(); held_color_.reset(); held_depth_.reset(); reset_scene();
    boundary_.reset(); frame_start_=true; scene_phase_=false; scene_restore_copy_.reset(); frame_reticle_.clear(); bloom_valid_=false;
    if(!width || !height) return;
    rebuild_output();
}
void D3D12Renderer::set_render_scale(double scale) {
    scale=device_render_scale(scale,16384,aspect_); if(recording_) { pending_scale_=scale; return; } if(scale_==scale) return; idle(); scale_=scale;
    if(device_) {
        if(generation_) generation_->release_context();
        upscaler_.reset();
        reset_scene();
        invalidate_held();
        for(const auto& item:copies_) lost_.insert(item.first);
        copies_.clear(); aliases_.clear(); latest_copies_.clear();
        recycled_copies_.clear(); recycled_copy_bytes_=0;
        scene_restore_copy_.reset(); boundary_.reset(); frame_start_=true; scene_phase_=false; bloom_valid_=false;
        const auto extent=manual_render_extent(scale_,aspect_); const auto height=uint32_t(std::llround(960*scale_)); efb_=image(extent.width,height,1,color_format,true,false,true); depth_=image(extent.width,height,1,DXGI_FORMAT_D32_FLOAT,false,true); efb_initialized_=false; last_efb_=0;
        configure_upscaler(); samplers();
        if(generation_) configure_generation();
    }
}
void D3D12Renderer::set_controls(const NativeControls& controls) {
    if(controls.crosshair_percent>100 || !std::isfinite(controls.wake_seconds) || controls.wake_seconds<0) throw std::invalid_argument("Invalid DirectX pointer controls");
    for(const auto level:{controls.effects.ambient_occlusion,controls.effects.shadows,controls.effects.motion_blur,controls.effects.rt_quality,controls.effects.rt_reflections,controls.effects.rt_lighting,controls.effects.rt_shadows,controls.effects.rt_local_shadows,controls.effects.rt_ambient_occlusion})
        if(unsigned(level)>unsigned(EffectLevel::ultra)) throw std::invalid_argument("Invalid DirectX effect quality");
    if(device_ && (controls.effects.ray_tracing || controls.effects.path_tracing || controls.effects.ray_reconstruction) && !rt_supported_) throw std::runtime_error("DirectX ray tracing requires DXR 1.1 and Shader Model 6.5");
    if(device_ && upscaler_kind_==UpscalerKind::dlss && controls.effects.ray_reconstruction && streamline_ && streamline_->ray_reconstruction_loaded() && !rr_supported_) throw std::runtime_error("DirectX DLSS Ray Reconstruction is unavailable on this adapter");
    if(recording_) { pending_controls_=controls; return; }
    NativeControls applied=controls;
    const bool requested_effects_changed=controls.scene_targets!=controls_.scene_targets || controls.effects!=controls_.effects;
    if(initialized_ && !frame_start_ && requested_effects_changed) {
        // Keep one scene's trackers, targets and TLAS consistent until its present.
        // Other controls apply now; each full update replaces or cancels the pending request.
        pending_controls_=controls; applied.scene_targets=controls_.scene_targets; applied.effects=controls_.effects;
    } else pending_controls_.reset();
    const bool rebuild=applied.anisotropy!=controls_.anisotropy;
    const bool effects_changed=applied.scene_targets!=controls_.scene_targets || applied.effects!=controls_.effects;
    if((rebuild || effects_changed) && device_) idle();
    if(effects_changed) { reset_scene(); invalidate_held(); }
    if(effects_changed && generation_) { generation_->release_context(); configure_generation(); }
    controls_=std::move(applied); effects_=effective_effects(controls_.effects,device_ ? rt_supported_ : true); if(rebuild && device_) samplers();
}
bool D3D12Renderer::copy_ready(const std::shared_ptr<const gx::TexturePixels>& pixels,bool previous) {
    if(!pixels || !pixels->gpu_only || pixels->levels.empty()) throw std::invalid_argument("Invalid DirectX copy readiness query");
    if(const auto found=copies_.find(pixels.get());found!=copies_.end() && found->second->source.lock()==pixels && found->second->written) return true;
    if(lost_.contains(pixels.get())) return true;
    if(const auto found=aliases_.find(pixels.get());found!=aliases_.end() && found->second.requested.lock()==pixels) return true;
    if(previous && pixels->guest_address) {
        std::shared_ptr<Image> image;
        if(const auto found=latest_copies_.find(pixels->guest_address);found!=latest_copies_.end()) image=found->second.lock();
        if(image) {
            const auto original=image->source.lock();
            if(!original || original->channels!=pixels->channels || original->levels.front().width!=pixels->levels.front().width || original->levels.front().height!=pixels->levels.front().height) image.reset();
        }
        aliases_[pixels.get()]={pixels,image}; return true;
    }
    return false;
}
std::vector<const gx::TexturePixels*> D3D12Renderer::copy_keys() const {
    std::vector<const gx::TexturePixels*> keys;
    for(const auto& [key,image]:copies_) if(!image->source.expired()) keys.push_back(key);
    for(const auto& [key,alias]:aliases_) if(!alias.requested.expired()) keys.push_back(key);
    keys.insert(keys.end(),lost_.begin(),lost_.end()); return keys;
}
std::vector<uint64_t> D3D12Renderer::poll_completions() { collect(); auto result=std::move(completed_); completed_.clear(); return result; }
bool D3D12Renderer::wait_for_pending(uint64_t timeout_ns) {
    collect(); uint64_t first=UINT64_MAX; for(const auto& slot:slots_) if(slot.fence) first=std::min(first,slot.fence);
    return first!=UINT64_MAX && wait(first,timeout_ns);
}
RenderPixels D3D12Renderer::capture(uint64_t serial,bool efb) {
    if(!serial || serial!=(efb ? last_efb_ : last_output_)) throw std::invalid_argument("DirectX capture serial is no longer retained");
    idle(); auto target=efb ? efb_ : output_;
    const auto desc=target->resource->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT rows=0; UINT64 row_bytes=0,total=0;
    device_->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&row_bytes,&total); auto readback=buffer(size_t(total),D3D12_HEAP_TYPE_READBACK);
    Slot* slot=begin(65536,texture_registers); if(!slot) throw std::logic_error("DirectX idle did not retire commands");
    transition(*slot,*target,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource=target->resource.Get(); source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource=readback.Get(); destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint=footprint;
    slot->command->CopyTextureRegion(&destination,0,0,0,&source,nullptr); wait(submit(*slot,0),UINT64_MAX);
    RenderPixels pixels; pixels.serial=serial; pixels.width=target->width; pixels.height=target->height; pixels.rgba.resize(size_t(pixels.width)*pixels.height*4);
    void* mapped=nullptr; D3D12_RANGE range{0,size_t(total)}; check(readback->Map(0,&range,&mapped),"Map DirectX readback");
    for(UINT row=0;row<rows;++row) std::memcpy(pixels.rgba.data()+size_t(row)*pixels.width*4,static_cast<std::byte*>(mapped)+footprint.Offset+size_t(row)*footprint.Footprint.RowPitch,size_t(row_bytes));
    D3D12_RANGE empty{}; readback->Unmap(0,&empty); return pixels;
}
std::vector<std::byte> D3D12Renderer::readback(Image& target) {
    idle();
    const auto desc=target.resource->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT rows=0; UINT64 row_bytes=0,total=0;
    device_->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&row_bytes,&total); auto staging=buffer(size_t(total),D3D12_HEAP_TYPE_READBACK);
    Slot* slot=begin(65536,texture_registers); if(!slot) throw std::logic_error("DirectX idle did not retire commands");
    transition(*slot,target,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource=target.resource.Get(); source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource=staging.Get(); destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint=footprint;
    slot->command->CopyTextureRegion(&destination,0,0,0,&source,nullptr); wait(submit(*slot,0),UINT64_MAX);
    std::vector<std::byte> result(size_t(row_bytes)*rows);
    void* mapped=nullptr; D3D12_RANGE range{0,size_t(total)}; check(staging->Map(0,&range,&mapped),"Map DirectX scene readback");
    for(UINT row=0;row<rows;++row) std::memcpy(result.data()+size_t(row)*size_t(row_bytes),static_cast<std::byte*>(mapped)+footprint.Offset+size_t(row)*footprint.Footprint.RowPitch,size_t(row_bytes));
    D3D12_RANGE empty{}; staging->Unmap(0,&empty); return result;
}
RenderPixels D3D12Renderer::capture_scene() {
    if(!scene_captured_ || !scene_color_) throw std::logic_error("No DirectX scene snapshot to capture");
    const auto bytes=readback(*scene_color_); RenderPixels result{last_efb_,scene_color_->width,scene_color_->height,{}};
    result.rgba.resize(bytes.size()); std::memcpy(result.rgba.data(),bytes.data(),bytes.size()); return result;
}
MotionPixels D3D12Renderer::capture_motion() {
    if(!scene_captured_ || !scene_motion_) throw std::logic_error("No DirectX motion snapshot to capture");
    const auto bytes=readback(*scene_motion_); MotionPixels result{scene_motion_->width,scene_motion_->height,{}};
    const auto half=[](uint16_t h) {
        const uint32_t sign=uint32_t(h>>15)<<31,exponent=(h>>10)&31,mantissa=h&1023;
        return !exponent ? std::bit_cast<float>(sign)+(sign ? -1.f : 1.f)*std::ldexp(float(mantissa),-24) : exponent==31 ? std::bit_cast<float>(sign|0x7F800000u|(mantissa<<13)) : std::bit_cast<float>(sign|((exponent+112)<<23)|(mantissa<<13));
    };
    result.rg.reserve(size_t(result.width)*result.height*2);
    for(size_t i=0;i<size_t(result.width)*result.height;++i) {
        uint16_t halves[2]; std::memcpy(halves,bytes.data()+i*8,sizeof halves); result.rg.push_back(half(halves[0])); result.rg.push_back(half(halves[1]));
    }
    return result;
}
NativeStats D3D12Renderer::stats() const {
    auto result=stats_;
    result.generated_presentations=retired_generated_+(generation_ ? generation_->generated() : 0);
    if(info_) for(UINT64 i=0;i<info_->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
        SIZE_T size=0; if(FAILED(info_->GetMessage(i,nullptr,&size))) continue;
        std::vector<std::byte> bytes(size); auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        if(SUCCEEDED(info_->GetMessage(i,message,&size)) && (message->Severity==D3D12_MESSAGE_SEVERITY_ERROR || message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION)) { ++result.validation_errors; std::fprintf(stderr,"[dx12 validation] %s\n",message->pDescription); }
    }
    return result;
}
void D3D12Renderer::shutdown() {
    if(!device_) return;
    idle(); initialized_=false;
    if(rt_effects_warm_.valid()) rt_effects_warm_.get();
    tev_.reset(); { std::lock_guard lock(spec_mutex_); spec_done_.clear(); } spec_pipelines_.clear(); spec_pending_.clear(); dxbc_vertex_.clear(); dxbc_motion_vertex_.clear();   // the compile threads first: they hold the device
    dlss5_pass_.shutdown(); dlss5_context_={}; dlss5_device_=false;
    if(generation_) generation_->release_context();
    upscaler_.reset();
    destroy_overlay();
    invalidate_held(); held_color_.reset(); held_depth_.reset();
    reset_scene(); motion_vertex_.clear(); motion_pixel_.clear(); logic_pixel_.clear(); motion_logic_pixel_.clear();
    for(auto& pipeline:effects_pipelines_) pipeline.Reset(); effects_root_.Reset();
    shadow_pipeline_.Reset();
    depth_blit_pipeline_.Reset();
    if(swapchain_ && exclusive_) check(swapchain_->SetFullscreenState(FALSE,nullptr),"Leave DirectX exclusive fullscreen");
    for(auto& slot:slots_) { if(slot.upload) slot.upload->Unmap(0,nullptr); slot={}; }
    rt_effects_.reset(); rt_scene_.shutdown(); rt_supported_=rr_supported_=false;
    textures_.clear(); copies_.clear(); aliases_.clear(); latest_copies_.clear(); meshes_.clear(); pipelines_.clear(); lost_.clear();
    recycled_copies_.clear(); recycled_copy_bytes_=0;
    for(auto& level:bloom_down_) level.reset(); for(auto& level:bloom_up_) level.reset(); bloom_color_.reset();
    bloom_region_={}; bloom_valid_=false;
    smaa_source_.reset(); smaa_edges_.reset(); smaa_weights_.reset(); smaa_output_.reset(); smaa_area_.reset(); smaa_search_.reset();
    for(auto& pipeline:smaa_pipelines_) pipeline.Reset(); smaa_root_.Reset();
    for(auto& pipeline:bloom_pipelines_) pipeline.Reset(); bloom_root_.Reset(); scene_restore_copy_.reset(); boundary_.reset(); frame_start_=true; scene_phase_=false;
    efb_.reset(); depth_.reset(); output_.reset(); white_.reset(); black_.reset(); blit_pipeline_.Reset();
    for(auto& resource:backbuffers_) resource.Reset(); backbuffer_views_.Reset(); hdr_pipeline_.Reset(); ui_image_.reset();
    hdr_active_=false; output_dirty_=hdr_metadata_dirty_=true;
    swapchain_.Reset();
    if(generation_) retired_generated_+=generation_->generated(); generation_.reset(); generation_kind_=UpscalerKind::off;
    hudless_={}; hudless_sdr_.reset();
    queries_.Reset(); root_.Reset(); samplers_.Reset(); queue_.Reset(); fence_.Reset(); info_.Reset();
    streamline_.reset(); device_.Reset();
    if(event_) { CloseHandle(event_); event_=nullptr; }
}
void D3D12Renderer::set_menu(std::function<void()> build,std::function<void()> setup) {
    if(ui_ && setup && !menu_setup_) { idle(); destroy_overlay(); }
    menu_build_=std::move(build); if(setup) menu_setup_=std::move(setup);
}
bool D3D12Renderer::menu_event(const SDL_Event& event) {
    if(!ui_) return false;
    auto* previous=ImGui::GetCurrentContext(); ImGui::SetCurrentContext(ui_);
    ImGui_ImplSDL3_ProcessEvent(&event);
    const auto& io=ImGui::GetIO(); const bool captured=io.WantCaptureMouse || io.WantCaptureKeyboard || io.WantTextInput;
    ImGui::SetCurrentContext(previous); return captured;
}
void D3D12Renderer::destroy_overlay() {
    if(!ui_) return;
    auto* previous=ImGui::GetCurrentContext(); ImGui::SetCurrentContext(ui_);
    if(ImGui::GetIO().BackendRendererUserData) ImGui_ImplDX12_Shutdown();
    if(ImGui::GetIO().BackendPlatformUserData) ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ui_); ImGui::SetCurrentContext(previous==ui_ ? nullptr : previous); ui_=nullptr;
    ui_heap_.Reset(); ui_allocated_.fill(false); ui_retired_.fill(0); font_bytes_.clear();
}
void D3D12Renderer::overlay(Slot& slot,const gx::FramePacket& packet,Image& image) {
    struct ContextRestore { ImGuiContext* previous=ImGui::GetCurrentContext(); ~ContextRestore() { ImGui::SetCurrentContext(previous); } } restore;
    if(!ui_) {
        ui_=ImGui::CreateContext(); ImGui::SetCurrentContext(ui_);
        auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.LogFilename=nullptr; io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
        if(menu_setup_) menu_setup_(); else load_prompt_font(font_bytes_);
        if(!ImGui_ImplSDL3_InitForD3D(window_)) throw std::runtime_error("DirectX overlay SDL initialization failed");
        ui_heap_=heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,UINT(ui_allocated_.size()),true);
        ImGui_ImplDX12_InitInfo init; init.Device=device_.Get(); init.CommandQueue=queue_.Get(); init.NumFramesInFlight=int(slot_count); init.RTVFormat=color_format;
        init.SrvDescriptorHeap=ui_heap_.Get(); init.UserData=this;
        init.SrvDescriptorAllocFn=[](ImGui_ImplDX12_InitInfo* info,D3D12_CPU_DESCRIPTOR_HANDLE* cpu,D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
            auto& self=*static_cast<D3D12Renderer*>(info->UserData); const auto completed=self.fence_->GetCompletedValue();
            for(size_t i=0;i<self.ui_allocated_.size();++i) if(!self.ui_allocated_[i] && self.ui_retired_[i]<=completed) {
                self.ui_allocated_[i]=true; *cpu=self.ui_heap_->GetCPUDescriptorHandleForHeapStart(); cpu->ptr+=i*self.srv_stride_;
                *gpu=self.ui_heap_->GetGPUDescriptorHandleForHeapStart(); gpu->ptr+=i*self.srv_stride_; return;
            }
            throw std::runtime_error("DirectX overlay descriptor pool exhausted");
        };
        init.SrvDescriptorFreeFn=[](ImGui_ImplDX12_InitInfo* info,D3D12_CPU_DESCRIPTOR_HANDLE cpu,D3D12_GPU_DESCRIPTOR_HANDLE) {
            auto& self=*static_cast<D3D12Renderer*>(info->UserData);
            const auto index=(cpu.ptr-self.ui_heap_->GetCPUDescriptorHandleForHeapStart().ptr)/self.srv_stride_;
            if(index>=self.ui_allocated_.size() || !self.ui_allocated_[index]) throw std::logic_error("Invalid DirectX overlay descriptor retirement");
            self.ui_allocated_[index]=false; self.ui_retired_[index]=self.next_fence_;
        };
        if(!ImGui_ImplDX12_Init(&init)) throw std::runtime_error("DirectX overlay GPU initialization failed");
    }
    ImGui::SetCurrentContext(ui_); ImGui_ImplDX12_NewFrame(); ImGui_ImplSDL3_NewFrame();
    auto& io=ImGui::GetIO(); io.DisplaySize={float(width_),float(height_)}; io.DisplayFramebufferScale={1,1};
    ImGui::NewFrame();
    if(!packet.black_output) for(const auto& prompt:packet.prompts) draw_prompt_picture(prompt,{width_,height_},packet.display_source,prompt_style_);
    if(menu_build_) menu_build_();
    ImGui::Render();
    transition(slot,image,D3D12_RESOURCE_STATE_RENDER_TARGET); const auto target=rtv(image); slot.command->OMSetRenderTargets(1,&target,FALSE,nullptr);
    ID3D12DescriptorHeap* heaps[]{ui_heap_.Get()}; slot.command->SetDescriptorHeaps(1,heaps); ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(),slot.command.Get());
}
}
std::unique_ptr<NativeRenderer> make_d3d12_renderer() { return std::make_unique<D3D12Renderer>(); }
}
