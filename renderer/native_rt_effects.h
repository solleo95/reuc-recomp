#pragma once
#include "native_upscaler.h"
#include "native_rt_scene.h"
#include "graphics_effects.h"
#include "rt_material.h"
#include "view_reconstruct.h"
#include <span>
#include <functional>

namespace uc {
struct NativeRtInputs {
    ID3D12GraphicsCommandList* command=nullptr;
    std::shared_ptr<NativeRtScene::Frame> scene;
    // Color enters/leaves UAV; all other inputs enter/leave NON_PIXEL_SHADER_RESOURCE.
    NativeUpscaleImage color,depth,motion,gbuffer,material,albedo;
    std::span<const RtMaterialRecord> records;
    std::span<const gx::LightUniform> lights;
    RenderExtent region{};
    std::array<float,6> viewport{};
    ViewParams view{};
    std::array<float,3> overhead{0,1,0};
    GraphicsEffects effects;
    uint32_t frame=0;
    bool reset=false,ray_reconstruction=false;
    std::function<void()> raster_occlusion;
};
// Retain through the submission fence, including RR evaluation that consumes these guides.
class NativeRtWork {
public:
    NativeUpscaleImage normal_roughness,specular_albedo;
    struct Impl;
    std::shared_ptr<Impl> storage;
};
class NativeRtEffects {
public:
    NativeRtEffects(ID3D12Device*,const std::filesystem::path& shaders);
    ~NativeRtEffects();
    NativeRtEffects(const NativeRtEffects&)=delete;
    NativeRtEffects& operator=(const NativeRtEffects&)=delete;
    std::shared_ptr<NativeRtWork> execute(const NativeRtInputs&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
