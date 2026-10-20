#include "upscaler.h"
#include "streamline.h"
#include "streamline_api.h"
#include "upscale_math.h"
#include <sl_matrix_helpers.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace uc {
namespace {
sl::DLSSMode dlss_mode(UpscaleMode mode) {
    switch(mode) {
    case UpscaleMode::native: return sl::DLSSMode::eDLAA;
    case UpscaleMode::quality: return sl::DLSSMode::eMaxQuality;
    case UpscaleMode::balanced: return sl::DLSSMode::eBalanced;
    case UpscaleMode::performance: return sl::DLSSMode::eMaxPerformance;
    case UpscaleMode::ultra_performance: return sl::DLSSMode::eUltraPerformance;
    }
    return sl::DLSSMode::eMaxQuality;
}
// Streamline matrices are row-major for row vectors (v * M): the transpose of the renderer's column-vector matrices (M * v).
sl::float4x4 to_sl(const gx::Projection& m) {
    sl::float4x4 r;
    for (uint32_t i=0;i<4;++i) r[i]=sl::float4(m[0][i],m[1][i],m[2][i],m[3][i]);
    return r;
}
sl::Resource image(const UpscaleImage& i) {
    sl::Resource r(sl::ResourceType::eTex2d, i.image, nullptr, i.view, VK_IMAGE_LAYOUT_GENERAL);
    r.width=i.extent.width; r.height=i.extent.height; r.nativeFormat=uint32_t(i.format); r.mipLevels=1; r.arrayLayers=1; r.flags=0; r.usage=i.usage;
    return r;
}
void check(sl::Result result, const char* what) {
    if (result!=sl::Result::eOk) throw std::runtime_error(std::string("DLSS: ")+what+" failed: "+sl::getResultAsStr(result));
}
class DlssUpscaler final : public Upscaler {
public:
    DlssUpscaler(const Streamline& streamline, UpscaleMode mode) : api_(streamline.api()), mode_(mode) {
        options_.mode=dlss_mode(mode);
        options_.dlaaPreset=options_.qualityPreset=options_.balancedPreset=sl::DLSSPreset(dlss_preset(UpscaleMode::quality));
        options_.performancePreset=sl::DLSSPreset(dlss_preset(UpscaleMode::performance));
        options_.ultraPerformancePreset=sl::DLSSPreset(dlss_preset(UpscaleMode::ultra_performance));
        options_.colorBuffersHDR=sl::Boolean::eFalse;   // the game is LDR (RGBA8)
        options_.preExposure=1.0f;
    }
    ~DlssUpscaler() override {
        api_.free_resources(sl::kFeatureDLSS, viewport_);
        if (rr_options_set_) api_.free_resources(sl::kFeatureDLSS_RR, viewport_);
    }
    void evaluate(const UpscaleInputs& in) override {
        sl::FrameToken* token=api_.frame;   // taken by the renderer for this guest frame (Streamline::begin_frame)
        if (!token) throw std::runtime_error("DLSS: no frame token");
        if (options_.outputWidth!=in.output_size.width || options_.outputHeight!=in.output_size.height) {
            options_.outputWidth=in.output_size.width; options_.outputHeight=in.output_size.height;
            check(UC_SL_CALL(sl::kFeatureDLSS,slDLSSSetOptions,viewport_,options_),"options");
            std::fprintf(stderr,"DLSS: %ux%u <- %ux%u preset %c\n",in.output_size.width,in.output_size.height,in.render_size.width,in.render_size.height,
                char('A'+dlss_preset(mode_)-1));
            rr_options_.outputWidth=0;   // the Ray Reconstruction options follow the size too
        }
        set_ray_reconstruction(in);
        sl::Constants constants{};
        constants.cameraViewToClip=to_sl(in.projection);
        sl::matrixFullInvert(constants.clipToCameraView,constants.cameraViewToClip);
        constants.clipToLensClip=to_sl({{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}});
        constants.clipToPrevClip=to_sl(in.clip_to_prev_clip);
        sl::matrixFullInvert(constants.prevClipToClip,constants.clipToPrevClip);
        // As added to the geometry (pixel space, y down): checked against the exact edge coverage of a still scene (renderer_upscale_smoke --dlss).
        constants.jitterOffset={in.jitter[0],in.jitter[1]};
        constants.mvecScale={1.0f/in.render_size.width,1.0f/in.render_size.height};   // vectors are in internal pixels
        constants.cameraPinholeOffset={0,0};
        constants.cameraPos={0,0,0}; constants.cameraUp={0,1,0}; constants.cameraRight={1,0,0}; constants.cameraFwd={0,0,-1};   // view space: GX looks down -z
        const auto& p=in.projection;   // GX perspective: z' = p22*z + p23, w = -z
        if (p[2][2]!=0 && p[1][1]!=0 && p[0][0]!=0) {
            constants.cameraNear=std::abs(p[2][3]/p[2][2]);
            constants.cameraFar=std::abs(p[2][3]/(p[2][2]+1.0f));
            constants.cameraFOV=2.0f*std::atan(1.0f/p[1][1]);
            constants.cameraAspectRatio=p[1][1]/p[0][0];
        }
        constants.depthInverted=sl::Boolean::eFalse;
        constants.cameraMotionIncluded=sl::Boolean::eTrue;
        constants.motionVectors3D=sl::Boolean::eFalse;
        constants.reset=in.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        constants.orthographicProjection=sl::Boolean::eFalse;
        constants.motionVectorsDilated=sl::Boolean::eFalse;
        constants.motionVectorsJittered=sl::Boolean::eFalse;
        check(api_.set_constants(constants,*token,viewport_),"constants");
        auto color=image(in.color), depth=image(in.depth), motion=image(in.motion), output=image(in.output);
        const sl::Extent render{0,0,in.render_size.width,in.render_size.height}, shown{0,0,in.output_size.width,in.output_size.height};
        // The scene targets stay untouched until the next frame's first scene pass (the renderer defers their clear), so they are valid until
        // present and Streamline neither copies them nor loses them for frame generation; the output gets the HUD drawn into it right after.
        const sl::ResourceTag tags[]={
            {&color,sl::kBufferTypeScalingInputColor,sl::ResourceLifecycle::eValidUntilPresent,&render},
            {&depth,sl::kBufferTypeDepth,sl::ResourceLifecycle::eValidUntilPresent,&render},
            {&motion,sl::kBufferTypeMotionVectors,sl::ResourceLifecycle::eValidUntilPresent,&render},
            {&output,sl::kBufferTypeScalingOutputColor,sl::ResourceLifecycle::eOnlyValidNow,&shown}};
        const sl::BaseStructure* inputs[]={&viewport_};
        if (in.ray_reconstruction) {   // Ray Reconstruction replaces Super Resolution for this frame: the same tags and the three guides
            auto diffuse=image(in.diffuse_albedo), specular=image(in.specular_albedo), normals=image(in.normal_roughness);
            const sl::ResourceTag rr_tags[]={tags[0],tags[1],tags[2],tags[3],
                {&diffuse,sl::kBufferTypeAlbedo,sl::ResourceLifecycle::eValidUntilPresent,&render},
                {&specular,sl::kBufferTypeSpecularAlbedo,sl::ResourceLifecycle::eValidUntilPresent,&render},
                {&normals,sl::kBufferTypeNormalRoughness,sl::ResourceLifecycle::eValidUntilPresent,&render}};
            check(api_.set_tag_for_frame(*token,viewport_,rr_tags,7,in.command),"RR tags");
            check(api_.evaluate_feature(sl::kFeatureDLSS_RR,*token,inputs,1,in.command),"RR evaluate");
            return;
        }
        check(api_.set_tag_for_frame(*token,viewport_,tags,4,in.command),"tags");
        check(api_.evaluate_feature(sl::kFeatureDLSS,*token,inputs,1,in.command),"evaluate");
    }
    bool sharpens() const override { return false; }
    RenderExtent minimum_internal(RenderExtent output) const override {
        auto options=options_; options.outputWidth=output.width; options.outputHeight=output.height;
        sl::DLSSOptimalSettings settings{};
        if (UC_SL_CALL(sl::kFeatureDLSS,slDLSSGetOptimalSettings,options,settings)!=sl::Result::eOk || !settings.renderWidthMin) return {1,1};
        // shortcut: the minimum is the scene viewport's; the EFB target is 4/3 taller (640x480 logical for a 640x360 scene), so it is conservative.
        return {settings.renderWidthMin,settings.renderHeightMin};
    }
private:
    // Ray Reconstruction is an extension of DLSS (it runs at the same quality mode): its options are set when the frame type changes (or the size did) and it is turned off
    // again when a frame goes through Super Resolution.
    void set_ray_reconstruction(const UpscaleInputs& in) {
        if (in.ray_reconstruction==rr_on_ && rr_options_.outputWidth==in.output_size.width && rr_options_.outputHeight==in.output_size.height) return;
        if (!in.ray_reconstruction && !rr_options_set_) { rr_on_=false; return; }   // never used: nothing to turn off
        rr_options_.mode=in.ray_reconstruction ? dlss_mode(mode_) : sl::DLSSMode::eOff;
        rr_options_.outputWidth=in.output_size.width; rr_options_.outputHeight=in.output_size.height;
        rr_options_.colorBuffersHDR=sl::Boolean::eTrue;   // the guide: Ray Reconstruction takes HDR inputs only; the game's display-range colour goes in as it is, exposure 1
        rr_options_.preExposure=1.0f; rr_options_.exposureScale=1.0f;
        rr_options_.normalRoughnessMode=sl::DLSSDNormalRoughnessMode::ePacked;   // roughness in the normals' alpha
        rr_options_.dlaaPreset=rr_options_.qualityPreset=rr_options_.balancedPreset=rr_options_.performancePreset=rr_options_.ultraPerformancePreset=rr_options_.ultraQualityPreset=sl::DLSSDPreset::ePresetD;
        check(UC_SL_CALL(sl::kFeatureDLSS_RR,slDLSSDSetOptions,viewport_,rr_options_),"RR options");
        rr_on_=in.ray_reconstruction; rr_options_set_=true;
        std::fprintf(stderr,"DLSS: Ray Reconstruction %s\n",rr_on_ ? "on" : "off");
    }
    const StreamlineApi& api_;
    UpscaleMode mode_;
    sl::ViewportHandle viewport_{0};
    sl::DLSSOptions options_{};
    sl::DLSSDOptions rr_options_{};
    bool rr_on_=false, rr_options_set_=false;
};
}
std::unique_ptr<Upscaler> make_dlss_upscaler(const Streamline& streamline, UpscaleMode mode) { return std::make_unique<DlssUpscaler>(streamline,mode); }
}
