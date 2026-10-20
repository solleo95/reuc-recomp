#include "ngx_dlss5.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>

namespace uc {
namespace {
bool ngx_failed(uint32_t result) { return (result & 0xFFF00000u)==0xBAD00000u; }   // NVSDK_NGX_FAILED
bool contains_any(std::string_view text, std::initializer_list<std::string_view> words) {
    std::string lower(text); std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c) { return char(std::tolower(c)); });
    return std::any_of(words.begin(),words.end(),[&](std::string_view word) { return lower.find(word)!=std::string::npos; });
}
std::string append_log(std::string text, std::string_view log_line) { return log_line.empty() ? text : text+": "+std::string(log_line); }
}
std::string ngx_result_name(uint32_t result) {
    static constexpr std::array<const char*,19> names{"","FeatureNotSupported","PlatformError","FeatureAlreadyExists","FeatureNotFound","InvalidParameter","ScratchBufferTooSmall","NotInitialized",
        "UnsupportedInputFormat","RWFlagMissing","MissingInput","UnableToInitializeFeature","OutOfDate","OutOfGPUMemory","UnsupportedFormat","UnableToWriteToAppDataPath","UnsupportedParameter","Denied","NotImplemented"};
    if(result==kNgxSuccess) return "Success";
    char hex[16]; std::snprintf(hex,sizeof hex,"0x%08x",result);
    if(!ngx_failed(result)) return std::string("result ")+hex;
    const uint32_t code=result&0xFFFFF;
    return std::string("FAIL")+(code>0 && code<names.size() ? std::string("_")+names[code] : std::string())+" ("+hex+")";
}
std::string dlss5_requirement_reason(const NgxRequirement& requirement) {
    std::string text;
    const auto add=[&](const std::string& part) { text+=(text.empty() ? "" : "; ")+part; };
    const uint32_t bits=requirement.unsupported_bits;
    if(bits&1) add("the driver cannot report DLSS 5 support");
    if(bits&2) add("the NVIDIA driver is too old for DLSS 5");
    if(bits&4) { char arch[16]; std::snprintf(arch,sizeof arch,"0x%x",requirement.min_architecture); add(requirement.min_architecture ? std::string("this GPU is too old for DLSS 5 (needs architecture ")+arch+")" : "this GPU does not support DLSS 5"); }
    if(bits&8) add("Windows is too old for DLSS 5"+(requirement.min_os.empty() ? std::string() : " (needs "+requirement.min_os+")"));
    if(bits&16) add("the driver has no DLSS 5 support");
    if(bits&~31u) add("DLSS 5 is not supported (code "+std::to_string(bits)+")");
    return text;
}
namespace {
std::string without_log_prefix(std::string_view line) {   // "[2026-10-20 08:32:12] [NGXValidateSnippetMetaData:1499] error: ..." -> "error: ..."
    while(!line.empty() && line.front()=='[') {
        const auto close=line.find(']');
        if(close==std::string_view::npos) break;
        line.remove_prefix(close+1);
        while(!line.empty() && line.front()==' ') line.remove_prefix(1);
    }
    return std::string(line);
}
}
std::string dlss5_failure_reason(const char* step, uint32_t result, std::string_view log_line) {
    std::string text=std::string("DLSS 5 ")+step+" failed: "+ngx_result_name(result);
    const auto detail=without_log_prefix(log_line);
    if(contains_any(detail,{"newer gpu arch","unsupported gpu architecture"})) text+=" (this "+std::string(kDlss5Runtime)+" is built for a newer GPU generation)";
    else if(contains_any(detail,{"not supported on this device"})) text+=" (usually this "+std::string(kDlss5Runtime)+" targets a newer GPU generation than this one)";   // the core refused the snippet while loading it; the cause is in debug/ngx_dlss5.log when NGX logged it
    return append_log(std::move(text),detail);
}
std::string ngx_informative_line(const std::vector<std::string>& recent) {
    // the strongest wording first: an error line beats a line that merely mentions an architecture
    for(const auto& words:{std::initializer_list<std::string_view>{"newer gpu","unsupported gpu","signature","not called"},std::initializer_list<std::string_view>{"error"},std::initializer_list<std::string_view>{"unsupported","corrupt","denied","fail"},std::initializer_list<std::string_view>{"snippet","missing","arch"}})
        for(auto line=recent.rbegin();line!=recent.rend();++line)
            if(contains_any(*line,words)) return *line;
    return recent.empty() ? std::string() : recent.back();
}
void dlss5_create_parameters(NgxParameters& p, const Dlss5FeatureKey& key) {
    p.set_uint("CreationNodeMask",1); p.set_uint("VisibilityNodeMask",1);
    p.set_uint("Width",key.width); p.set_uint("Height",key.height);
    p.set_uint("DLSSNR.Width",key.width); p.set_uint("DLSSNR.Height",key.height);
    p.set_uint("DLSSNR.Hint.Render.Preset",uint32_t(key.preset_hint));
}
void dlss5_run_parameters(NgxParameters& p, const Dlss5Run& run) {
    p.set_uint("CreationNodeMask",1); p.set_uint("VisibilityNodeMask",1);
    const auto resource=[&](const char* name, const NgxImage& image, VkExtent2D region) {
        const std::string base(name);
        p.set_image(name,image);
        p.set_uint((base+"SubrectBaseX").c_str(),0); p.set_uint((base+"SubrectBaseY").c_str(),0);
        p.set_uint((base+"SubrectWidth").c_str(),region.width); p.set_uint((base+"SubrectHeight").c_str(),region.height);
    };
    resource("DLSSNR.Color",run.color,run.color_region); resource("DLSSNR.Depth",run.depth,run.depth_region);
    resource("DLSSNR.MVec",run.motion,run.motion_region); resource("DLSSNR.Output",run.output,run.output_region);
    p.set_float("DLSSNR.JitterOffsetX",run.jitter[0]); p.set_float("DLSSNR.JitterOffsetY",run.jitter[1]);
    p.set_float("DLSSNR.MVecScaleX",run.motion_scale[0]); p.set_float("DLSSNR.MVecScaleY",run.motion_scale[1]);
    p.set_int("DLSSNR.DepthInverted",0); p.set_int("DLSSNR.Reset",run.reset ? 1 : 0); p.set_int("DLSSNR.Enabled",1);
    const auto& s=run.settings;
    p.set_float("DLSSNR.Intensity",float(s.intensity)); p.set_float("DLSSNR.GlobalToneStrength",float(s.tone)); p.set_float("DLSSNR.LocalToneStrength",float(s.tone));   // one Tone Intensity drives both
    p.set_float("DLSSNR.LocalStructureStrength",float(s.structure)); p.set_float("DLSSNR.SkinStructureStrength",float(s.character_intensity));
    p.set_int("DLSSNR.UseAutoMask",s.character_mask ? 1 : 0); p.set_int("DLSSNR.MaskBlendMode",0);
    p.set_uint("DLSSNR.Style",uint32_t(s.model));
    p.set_int("DLSSNR.UICorrection",s.ui_correction==Dlss5UiCorrection::on && s.hook==Dlss5Hook::present ? 1 : 0);   // auto and off leave it disabled; only a frame that contains the HUD has a UI to correct
    p.set_float("DLSSNR.ScalingRatio",1.f);   // the bridge already brought the picture to the network's size
}
std::unique_ptr<Dlss5Runtime> Dlss5Runtime::open(std::unique_ptr<NgxBackend> backend, const NgxInit& init, std::string& reason) {
    std::error_code ignored;
    reason.clear();
    if(!std::filesystem::exists(init.runtime_directory/kDlss5Runtime,ignored)) { reason=std::string(kDlss5Runtime)+" missing next to the game"; return nullptr; }
    if(const auto result=backend->init(init); ngx_failed(result)) { reason=dlss5_failure_reason("init",result,backend->diagnostic()); backend->shutdown(); return nullptr; }
    NgxRequirement requirement;
    // NotImplemented: the core cannot answer for this snippet (it refuses an unsuitable one while loading it); creating the feature then gives the authoritative result and the reason
    if(const auto result=backend->requirements(requirement); ngx_failed(result) && result!=0xBAD00012u) { reason=dlss5_failure_reason("support check",result,backend->diagnostic()); backend->shutdown(); return nullptr; }
    if(const auto why=dlss5_requirement_reason(requirement); !why.empty()) { reason=why; backend->shutdown(); return nullptr; }
    std::unique_ptr<Dlss5Runtime> runtime(new Dlss5Runtime);
    runtime->backend_=std::move(backend);
    return runtime;
}
Dlss5Runtime::~Dlss5Runtime() {
    release();
    run_parameters_.reset();   // before the session closes
    if(backend_) backend_->shutdown();
}
void Dlss5Runtime::release() {
    for(void* handle:handles_) backend_->release_feature(handle);
    handles_.clear();
}
bool Dlss5Runtime::prepare(VkCommandBuffer command, const Dlss5FeatureKey& key, uint32_t passes, const std::function<void()>& idle) {
    if(key==key_ && passes==wanted_passes_ && (failed_ || handles_.size()==passes)) return !failed_;
    if(!handles_.empty()) { if(idle) idle(); release(); }   // the old features may still be in flight
    key_=key; wanted_passes_=passes; failed_=false; reason_.clear();
    for(uint32_t pass=0;pass<passes;++pass) {
        uint32_t result=kNgxSuccess;
        auto parameters=backend_->allocate_parameters(result);
        if(ngx_failed(result) || !parameters) { reason_=dlss5_failure_reason("parameters",ngx_failed(result) ? result : 0xBAD00001u,backend_->diagnostic()); break; }
        dlss5_create_parameters(*parameters,key);
        void* handle=nullptr;
        result=backend_->create_feature(command,*parameters,handle);
        if(ngx_failed(result) || !handle) { reason_=dlss5_failure_reason("create",ngx_failed(result) ? result : 0xBAD0000Bu,backend_->diagnostic()); break; }
        handles_.push_back(handle);
    }
    if(!reason_.empty()) { release(); failed_=true; return false; }
    return true;
}
bool Dlss5Runtime::evaluate(VkCommandBuffer command, uint32_t pass, const Dlss5Run& run) {
    if(failed_ || pass>=handles_.size()) return false;
    uint32_t result=kNgxSuccess;
    if(!run_parameters_) run_parameters_=backend_->allocate_parameters(result);   // every run sets all of them again
    if(ngx_failed(result) || !run_parameters_) { reason_=dlss5_failure_reason("parameters",ngx_failed(result) ? result : 0xBAD00001u,backend_->diagnostic()); failed_=true; return false; }
    dlss5_run_parameters(*run_parameters_,run);
    result=backend_->evaluate(command,handles_[pass],*run_parameters_);
    if(ngx_failed(result)) { reason_=dlss5_failure_reason("evaluate",result,backend_->diagnostic()); failed_=true; return false; }
    return true;
}
}
