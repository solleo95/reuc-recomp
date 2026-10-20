#include "check.h"
#include "ngx_dlss5.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <variant>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
using namespace uc;
namespace fs=std::filesystem;
namespace {
using Value=std::variant<uint32_t,int,float,std::string>;
struct Recording final : NgxParameters {
    std::map<std::string,Value> values;
    void set_uint(const char* n, uint32_t v) override { values[n]=v; }
    void set_int(const char* n, int v) override { values[n]=v; }
    void set_float(const char* n, float v) override { values[n]=v; }
    void set_image(const char* n, const NgxImage& i) override { values[n]=std::to_string(i.extent.width)+"x"+std::to_string(i.extent.height)+(i.read_write ? " rw" : " ro"); }
    uint32_t u(const std::string& n) const { return std::get<uint32_t>(values.at(n)); }
    int i(const std::string& n) const { return std::get<int>(values.at(n)); }
    float f(const std::string& n) const { return std::get<float>(values.at(n)); }
    const std::string& s(const std::string& n) const { return std::get<std::string>(values.at(n)); }
};
// What the runtime did to the backend; it outlives the backend, which the runtime owns.
struct Stats {
    int inits=0, creates=0, evaluates=0, releases=0, shutdowns=0;
    std::vector<Recording> created, evaluated;
    std::vector<void*> released, evaluated_handles, created_handles;
} stats;
struct Config {
    uint32_t init_result=kNgxSuccess, requirement_result=kNgxSuccess, create_result=kNgxSuccess, evaluate_result=kNgxSuccess;
    NgxRequirement requirement;
    std::string log;
} config;
struct Fake final : NgxBackend {
    uint32_t init(const NgxInit&) override { ++stats.inits; return config.init_result; }
    uint32_t requirements(NgxRequirement& out) override { out=config.requirement; return config.requirement_result; }
    std::unique_ptr<NgxParameters> allocate_parameters(uint32_t& result) override { result=kNgxSuccess; return std::make_unique<Recording>(); }
    uint32_t create_feature(VkCommandBuffer, NgxParameters& p, void*& handle) override {
        ++stats.creates; stats.created.push_back(static_cast<Recording&>(p));
        if(config.create_result!=kNgxSuccess) return config.create_result;
        handle=reinterpret_cast<void*>(uintptr_t(0x100+stats.creates)); stats.created_handles.push_back(handle); return kNgxSuccess;
    }
    uint32_t evaluate(VkCommandBuffer, void* handle, NgxParameters& p) override { ++stats.evaluates; stats.evaluated.push_back(static_cast<Recording&>(p)); stats.evaluated_handles.push_back(handle); return config.evaluate_result; }
    void release_feature(void* handle) override { ++stats.releases; stats.released.push_back(handle); }
    void shutdown() override { ++stats.shutdowns; }
    std::string diagnostic() const override { return config.log; }
};
std::unique_ptr<Dlss5Runtime> open(const fs::path& directory, std::string& reason) {
    NgxInit init; init.runtime_directory=directory;
    return Dlss5Runtime::open(std::make_unique<Fake>(),init,reason);
}
void reset() { stats=Stats{}; config=Config{}; }
}
int main() {
    const auto directory=fs::temp_directory_path()/("uc-ngx-dlss5-"+std::to_string(GetCurrentProcessId()));
    fs::remove_all(directory); fs::create_directories(directory);
    const auto missing=directory/"empty"; fs::create_directories(missing);
    { std::ofstream(directory/kDlss5Runtime,std::ios::binary) << "x"; }   // only its presence matters to the check; the fake stands in for NGX
    std::string reason;
    // The runtime file is not next to the exe: a clear reason and NGX is never started
    reset(); CHECK(!open(missing,reason) && reason.find(kDlss5Runtime)!=std::string::npos && stats.inits==0);
    // NGX refuses to start
    reset(); config.init_result=0xBAD00007; CHECK(!open(directory,reason) && reason.find("NotInitialized")!=std::string::npos && stats.inits==1);
    // Requirement bits become sentences; the session is closed again when the GPU is refused
    reset(); config.requirement.unsupported_bits=2;
    CHECK(!open(directory,reason) && reason.find("driver")!=std::string::npos && stats.shutdowns==1);
    { NgxRequirement r; r.unsupported_bits=4; r.min_architecture=0x190; CHECK(dlss5_requirement_reason(r).find("GPU")!=std::string::npos); }
    { NgxRequirement r; r.unsupported_bits=8; r.min_os="10.0.22621"; const auto text=dlss5_requirement_reason(r); CHECK(text.find("Windows")!=std::string::npos && text.find("10.0.22621")!=std::string::npos); }
    { NgxRequirement r; r.unsupported_bits=1<<9; CHECK(!dlss5_requirement_reason(r).empty()); NgxRequirement ok; CHECK(dlss5_requirement_reason(ok).empty()); }
    reset(); config.requirement_result=0xBAD00001; CHECK(!open(directory,reason) && reason.find("FeatureNotSupported")!=std::string::npos);
    reset(); config.requirement_result=0xBAD00012; CHECK(open(directory,reason) && reason.empty());   // "cannot answer": CreateFeature decides
    // The names of results and the line that explains a failure
    CHECK(ngx_result_name(0xBAD0000B)=="FAIL_UnableToInitializeFeature (0xbad0000b)");
    CHECK(ngx_result_name(0xBAD01234)=="FAIL (0xbad01234)" && ngx_result_name(1)=="Success");
    CHECK(ngx_informative_line({"loaded module x","DLSSNR: Unsupported GPU architecture 0x190, minimum required 0x1b0","cleanup done"}).find("Unsupported GPU architecture")!=std::string::npos);
    CHECK(ngx_informative_line({"a","b"})=="b" && ngx_informative_line({}).empty());
    // a line that only mentions the architecture does not hide the error line before it, and the log prefix is cut from the reason
    {
        const std::vector<std::string> recent{"[t] [NGXValidateSnippetMetaData:1499] error: snippet is using newer GPU arch 1B0 > 190","[t] [tid:1] m_gpuArch = 0x190","[t] [x] loaded"};
        CHECK(ngx_informative_line(recent)==recent[0]);
        const auto text=dlss5_failure_reason("support check",0xBAD00012,recent[0]);
        CHECK(text.find("FAIL_NotImplemented")!=std::string::npos && text.find("error: snippet is using newer GPU arch 1B0 > 190")!=std::string::npos && text.find("[NGXValidate")==std::string::npos && text.find("newer GPU generation")!=std::string::npos);
    }
    { const auto text=dlss5_failure_reason("create",0xBAD0000B,"[t] [NVSDK_NGX_CreateFeature_Validate:724] error: feature is not supported on this device"); CHECK(text.find("targets a newer GPU generation")!=std::string::npos && text.find("error: feature is not supported on this device")!=std::string::npos); }
    // Feature creation fails on a GPU older than the DLL: the reason carries NGX's own words, and the same key is not tried again
    {
        reset(); config.create_result=0xBAD0000B; config.log="DLSSNR: snippet is using newer GPU arch 0x1b0 than 0x190";
        auto runtime=open(directory,reason); CHECK(runtime && reason.empty());
        const Dlss5FeatureKey key{1280,720,Dlss5Preset::p2};
        CHECK(!runtime->prepare(nullptr,key,1,[]{}));
        CHECK(runtime->reason().find("UnableToInitializeFeature")!=std::string::npos && runtime->reason().find("newer GPU")!=std::string::npos && stats.creates==1);
        CHECK(!runtime->prepare(nullptr,key,1,[]{}) && stats.creates==1);
        CHECK(!runtime->evaluate(nullptr,0,Dlss5Run{}) && stats.evaluates==0);   // no feature: nothing is evaluated
        config.create_result=kNgxSuccess;
        CHECK(runtime->prepare(nullptr,Dlss5FeatureKey{1280,720,Dlss5Preset::p3},1,[]{}) && stats.creates==2);   // another key tries again
    }
    // Creation: one feature per pass, the create parameters
    {
        reset(); auto runtime=open(directory,reason); CHECK(runtime); int idles=0;
        const Dlss5FeatureKey key{960,540,Dlss5Preset::h};
        CHECK(runtime->prepare(nullptr,key,2,[&]{ ++idles; }) && runtime->passes()==2 && stats.creates==2 && idles==0);
        const auto& created=stats.created[0];
        CHECK(created.u("DLSSNR.Width")==960 && created.u("DLSSNR.Height")==540 && created.u("Width")==960 && created.u("Height")==540);
        CHECK(created.u("DLSSNR.Hint.Render.Preset")==uint32_t(dlss5_preset_hint(Dlss5Preset::h)) && created.u("CreationNodeMask")==1 && created.u("VisibilityNodeMask")==1);
        CHECK(runtime->prepare(nullptr,key,2,[&]{ ++idles; }) && stats.creates==2);   // unchanged: kept
        // another size: the device is idled, the old features released, new ones made
        CHECK(runtime->prepare(nullptr,Dlss5FeatureKey{640,360,Dlss5Preset::h},1,[&]{ ++idles; }) && idles==1 && stats.releases==2 && stats.creates==3 && runtime->passes()==1);
        // Evaluate: the parameter names the DLL reads
        Dlss5Run run;
        run.color={VK_NULL_HANDLE,VK_NULL_HANDLE,VK_FORMAT_R16G16B16A16_SFLOAT,{640,360},false}; run.output={VK_NULL_HANDLE,VK_NULL_HANDLE,VK_FORMAT_R16G16B16A16_SFLOAT,{640,360},true};
        run.depth={VK_NULL_HANDLE,VK_NULL_HANDLE,VK_FORMAT_D32_SFLOAT,{1280,720},false}; run.motion={VK_NULL_HANDLE,VK_NULL_HANDLE,VK_FORMAT_R16G16_SFLOAT,{1280,720},false};
        run.color_region=run.output_region={640,360}; run.depth_region=run.motion_region={1280,720};
        run.jitter[0]=.25f; run.jitter[1]=-.5f; run.motion_scale[0]=.5f; run.motion_scale[1]=.5f; run.reset=true;
        run.settings.intensity=.5; run.settings.tone_global=.25; run.settings.tone_local=.75; run.settings.structure_local=.1; run.settings.structure_skin=.9;
        run.settings.auto_mask=true; run.settings.style=2; run.settings.ui_correction=Dlss5UiCorrection::on;
        CHECK(runtime->evaluate(nullptr,0,run) && stats.evaluates==1 && stats.evaluated_handles[0]==stats.created_handles[2]);
        const auto& p=stats.evaluated[0];
        CHECK(p.s("DLSSNR.Color")=="640x360 ro" && p.s("DLSSNR.Output")=="640x360 rw" && p.s("DLSSNR.Depth")=="1280x720 ro" && p.s("DLSSNR.MVec")=="1280x720 ro");
        for(const char* base:{"DLSSNR.ColorSubrectBase","DLSSNR.DepthSubrectBase","DLSSNR.MVecSubrectBase","DLSSNR.OutputSubrectBase"}) CHECK(p.u(std::string(base)+"X")==0 && p.u(std::string(base)+"Y")==0);
        CHECK(p.u("DLSSNR.ColorSubrectWidth")==640 && p.u("DLSSNR.ColorSubrectHeight")==360 && p.u("DLSSNR.OutputSubrectWidth")==640 && p.u("DLSSNR.OutputSubrectHeight")==360);
        CHECK(p.u("DLSSNR.DepthSubrectWidth")==1280 && p.u("DLSSNR.DepthSubrectHeight")==720 && p.u("DLSSNR.MVecSubrectWidth")==1280 && p.u("DLSSNR.MVecSubrectHeight")==720);
        CHECK(p.f("DLSSNR.JitterOffsetX")==.25f && p.f("DLSSNR.JitterOffsetY")==-.5f && p.f("DLSSNR.MVecScaleX")==.5f && p.f("DLSSNR.MVecScaleY")==.5f);
        CHECK(p.i("DLSSNR.Reset")==1 && p.i("DLSSNR.DepthInverted")==0 && p.i("DLSSNR.Enabled")==1);
        CHECK(p.f("DLSSNR.Intensity")==.5f && p.f("DLSSNR.GlobalToneStrength")==.25f && p.f("DLSSNR.LocalToneStrength")==.75f && p.f("DLSSNR.LocalStructureStrength")==.1f && p.f("DLSSNR.SkinStructureStrength")==.9f);
        CHECK(p.i("DLSSNR.UseAutoMask")==1 && p.i("DLSSNR.MaskBlendMode")==0 && p.u("DLSSNR.Style")==2 && p.i("DLSSNR.UICorrection")==1 && p.f("DLSSNR.ScalingRatio")==1.f);
        run.settings.ui_correction=Dlss5UiCorrection::automatic; run.settings.auto_mask=false; run.reset=false;
        CHECK(runtime->evaluate(nullptr,0,run));
        const auto& q=stats.evaluated[1];
        CHECK(q.i("DLSSNR.UICorrection")==0 && q.i("DLSSNR.UseAutoMask")==0 && q.i("DLSSNR.Reset")==0);
        CHECK(!runtime->evaluate(nullptr,1,run));   // there is only pass 0
        config.evaluate_result=0xBAD00005;
        CHECK(!runtime->evaluate(nullptr,0,run) && runtime->reason().find("evaluate")!=std::string::npos && runtime->reason().find("InvalidParameter")!=std::string::npos);
    }
    // Destroying the runtime releases the features and closes the NGX session
    CHECK(stats.releases==3 && stats.shutdowns==1);
    fs::remove_all(directory);
    return 0;
}
