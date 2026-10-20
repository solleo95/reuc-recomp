// The NGX SDK (nvsdk_ngx_d.lib) behind NgxBackend. NGX's driver core loads nvngx_dlssnr.dll from the path list and checks its signature and caller itself;
// this file only calls the SDK's public Vulkan entry points.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "ngx_dlss5.h"
#include <nvsdk_ngx_vk.h>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>

namespace uc {
namespace {
constexpr const char* kProjectId = "3f5c9a1e-6b2d-4e8a-9c7f-1d2b3a4c5e6f";   // the Streamline integration's project id: one application
constexpr NVSDK_NGX_Feature kFeature = static_cast<NVSDK_NGX_Feature>(kNgxFeatureDlss5);

// NGX's log callback has no user pointer: one process-wide sink writes debug/<name>.log and keeps the last lines for failure reasons.
class NgxLog {
public:
    void open(const std::filesystem::path& file) {
        std::lock_guard lock(mutex_);
        if(file.empty() || file_.is_open()) return;
        std::error_code ignored; std::filesystem::create_directories(file.parent_path(),ignored);
        file_.open(file,std::ios::app);
        if(file_.is_open()) file_ << "--- NGX session " << std::chrono::system_clock::now().time_since_epoch().count() << " ---\n" << std::flush;
    }
    void add(const char* message, int level, int feature) {
        std::string line(message ? message : "");
        while(!line.empty() && (line.back()=='\n' || line.back()=='\r')) line.pop_back();
        std::lock_guard lock(mutex_);
        if(file_.is_open()) file_ << "[" << level << "/" << feature << "] " << line << "\n" << std::flush;
        recent_.push_back(line); if(recent_.size()>512) recent_.pop_front();   // the core loads every feature's snippet after the one that failed: the cause is not among the last few lines
    }
    std::string informative() { std::lock_guard lock(mutex_); return ngx_informative_line({recent_.begin(),recent_.end()}); }
private:
    std::mutex mutex_; std::ofstream file_; std::deque<std::string> recent_;
};
NgxLog& ngx_log() { static NgxLog log; return log; }
void NVSDK_CONV log_callback(const char* message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature feature) { ngx_log().add(message,int(level),int(feature)); }

struct Discovery {   // what the support and extension queries take
    std::wstring directory, data;
    const wchar_t* paths[1]{};
    NVSDK_NGX_FeatureCommonInfo info{};
    NVSDK_NGX_FeatureDiscoveryInfo discovery{};
    explicit Discovery(const std::filesystem::path& runtime_directory, const std::filesystem::path& data_directory=std::filesystem::path()) {
        directory=std::filesystem::absolute(runtime_directory).wstring();
        data=std::filesystem::absolute(data_directory.empty() ? runtime_directory : data_directory).wstring();
        paths[0]=directory.c_str();
        info.PathListInfo.Path=paths; info.PathListInfo.Length=1;
        info.LoggingInfo.LoggingCallback=log_callback; info.LoggingInfo.MinimumLoggingLevel=NVSDK_NGX_LOGGING_LEVEL_ON; info.LoggingInfo.DisableOtherLoggingSinks=true;   // our callback writes debug/*.log; nothing goes to the console
        discovery.SDKVersion=NVSDK_NGX_Version_API; discovery.FeatureID=kFeature;
        discovery.Identifier.IdentifierType=NVSDK_NGX_Application_Identifier_Type_Project_Id;
        discovery.Identifier.v.ProjectDesc={kProjectId,NVSDK_NGX_ENGINE_TYPE_CUSTOM,"uc-recomp"};
        discovery.ApplicationDataPath=data.c_str(); discovery.FeatureInfo=&info;
    }
    Discovery(const Discovery&)=delete; Discovery& operator=(const Discovery&)=delete;
};
std::vector<std::string> names(uint32_t count, const VkExtensionProperties* properties) {
    std::vector<std::string> result;
    for(uint32_t i=0;i<count && properties;++i) result.emplace_back(properties[i].extensionName);
    return result;
}

class Parameters final : public NgxParameters {
public:
    explicit Parameters(NVSDK_NGX_Parameter* parameters) : parameters_(parameters) {}
    ~Parameters() override { NVSDK_NGX_VULKAN_DestroyParameters(parameters_); }
    NVSDK_NGX_Parameter* get() const { return parameters_; }
    void set_uint(const char* name, uint32_t value) override { NVSDK_NGX_Parameter_SetUI(parameters_,name,value); }
    void set_int(const char* name, int value) override { NVSDK_NGX_Parameter_SetI(parameters_,name,value); }
    void set_float(const char* name, float value) override { NVSDK_NGX_Parameter_SetF(parameters_,name,value); }
    void set_image(const char* name, const NgxImage& image) override {
        NVSDK_NGX_Resource_VK resource{};
        resource.Type=NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW; resource.ReadWrite=image.read_write;
        resource.Resource.ImageViewInfo={image.view,image.image,{image.aspect,0,1,0,1},image.format,image.extent.width,image.extent.height};
        resources_.push_back(resource);   // the pointer stays valid until the call that reads it: deque entries do not move
        NVSDK_NGX_Parameter_SetVoidPointer(parameters_,name,&resources_.back());
        if(resources_.size()>16) resources_.pop_front();
    }
private:
    NVSDK_NGX_Parameter* parameters_;
    std::deque<NVSDK_NGX_Resource_VK> resources_;
};

class Backend final : public NgxBackend {
public:
    uint32_t init(const NgxInit& in) override {
        ngx_log().open(in.log_file);
        discovery_=std::make_unique<Discovery>(in.runtime_directory,in.log_file.has_parent_path() ? in.log_file.parent_path() : std::filesystem::path());
        discovery_->info.LoggingInfo.MinimumLoggingLevel=in.verbose ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_ON;
        instance_=in.instance; physical_=in.physical; device_=in.device;
        const auto result=NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId,NVSDK_NGX_ENGINE_TYPE_CUSTOM,"uc-recomp",discovery_->data.c_str(),in.instance,in.physical,in.device,
            in.instance_proc,in.device_proc,&discovery_->info,NVSDK_NGX_Version_API);
        started_=NVSDK_NGX_SUCCEED(result);
        return uint32_t(result);
    }
    uint32_t requirements(NgxRequirement& out) override {
        NVSDK_NGX_FeatureRequirement requirement{};
        const auto result=NVSDK_NGX_VULKAN_GetFeatureRequirements(instance_,physical_,&discovery_->discovery,&requirement);
        out.unsupported_bits=uint32_t(requirement.FeatureSupported); out.min_architecture=requirement.MinHWArchitecture; out.min_os=requirement.MinOSVersion;
        return uint32_t(result);
    }
    std::unique_ptr<NgxParameters> allocate_parameters(uint32_t& result) override {
        NVSDK_NGX_Parameter* parameters=nullptr;
        result=uint32_t(NVSDK_NGX_VULKAN_AllocateParameters(&parameters));
        if(NVSDK_NGX_FAILED(NVSDK_NGX_Result(result)) || !parameters) return nullptr;
        return std::make_unique<Parameters>(parameters);
    }
    uint32_t create_feature(VkCommandBuffer command, NgxParameters& parameters, void*& handle) override {
        NVSDK_NGX_Handle* created=nullptr;
        const auto result=NVSDK_NGX_VULKAN_CreateFeature1(device_,command,kFeature,static_cast<Parameters&>(parameters).get(),&created);
        handle=created;
        return uint32_t(result);
    }
    uint32_t evaluate(VkCommandBuffer command, void* handle, NgxParameters& parameters) override {
        return uint32_t(NVSDK_NGX_VULKAN_EvaluateFeature_C(command,static_cast<NVSDK_NGX_Handle*>(handle),static_cast<Parameters&>(parameters).get(),nullptr));
    }
    void release_feature(void* handle) override { NVSDK_NGX_VULKAN_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(handle)); }
    void shutdown() override { if(started_) { NVSDK_NGX_VULKAN_Shutdown1(device_); started_=false; } }
    std::string diagnostic() const override { return ngx_log().informative(); }
    ~Backend() override { shutdown(); }
private:
    std::unique_ptr<Discovery> discovery_;
    VkInstance instance_=VK_NULL_HANDLE; VkPhysicalDevice physical_=VK_NULL_HANDLE; VkDevice device_=VK_NULL_HANDLE;
    bool started_=false;
};
}
std::unique_ptr<NgxBackend> make_ngx_backend() { return std::make_unique<Backend>(); }
std::vector<std::string> ngx_dlss5_instance_extensions(const std::filesystem::path& runtime_directory) {
    Discovery discovery(runtime_directory);
    uint32_t count=0; VkExtensionProperties* properties=nullptr;
    if(NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discovery.discovery,&count,&properties))) return {};
    return names(count,properties);
}
std::vector<std::string> ngx_dlss5_device_extensions(VkInstance instance, VkPhysicalDevice physical, const std::filesystem::path& runtime_directory) {
    Discovery discovery(runtime_directory);
    uint32_t count=0; VkExtensionProperties* properties=nullptr;
    if(NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance,physical,&discovery.discovery,&count,&properties))) return {};
    return names(count,properties);
}
}
