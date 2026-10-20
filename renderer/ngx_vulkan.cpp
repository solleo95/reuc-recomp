// DLSS 5 uses the snippet exports directly, like RenoDX: local custom runtimes need not pass the driver's signed-snippet loader.
// The NGX SDK still owns parameter allocation; its other features keep their normal signed loading path.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "ngx_dlss5.h"
#include <nvsdk_ngx_vk.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
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

// Only nvngx_dlssnr's import sees this identity. Windows and the other NGX/Streamline modules keep their real module names.
DWORD WINAPI snippet_module_name(HMODULE module, LPWSTR name, DWORD size) {
    if(module!=GetModuleHandleW(nullptr) || !name || !size) return GetModuleFileNameW(module,name,size);
    constexpr wchar_t core[]=L"nvngx.dll";
    if(size<std::size(core)) { name[0]=0; SetLastError(ERROR_INSUFFICIENT_BUFFER); return size; }
    std::wmemcpy(name,core,std::size(core)); return DWORD(std::size(core)-1);
}
std::mutex snippet_mutex;   // NGX entry points and import updates are not thread safe
class Snippet {
public:
    explicit Snippet(const std::filesystem::path& directory) {
        const auto file=std::filesystem::absolute(directory/kDlss5Runtime);
        module_=LoadLibraryExW(file.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!module_) { const DWORD code=GetLastError(); error_="cannot load "+file.string()+" (Windows error "+std::to_string(code)+")"; return; }
        bind(init,"NVSDK_NGX_VULKAN_Init_Ext2"); bind(create,"NVSDK_NGX_VULKAN_CreateFeature1");
        bind(evaluate,"NVSDK_NGX_VULKAN_EvaluateFeature"); bind(release,"NVSDK_NGX_VULKAN_ReleaseFeature");
        bind(shutdown,"NVSDK_NGX_VULKAN_Shutdown1");
        bind(instance_extensions,"NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements");
        bind(device_extensions,"NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements");
        if(error_.empty() && !identify_caller()) error_="nvngx_dlssnr.dll caller-identity import could not be installed";
    }
    ~Snippet() { if(module_) FreeLibrary(module_); }
    Snippet(const Snippet&)=delete; Snippet& operator=(const Snippet&)=delete;
    bool ready() const { return error_.empty(); }
    const std::string& error() const { return error_; }
    // Init_Ext2 is the snippet ABI, distinct from NVSDK_NGX_VULKAN_Init in the application SDK.
    NVSDK_NGX_Result (NVSDK_CONV *init)(unsigned long long,const wchar_t*,VkInstance,VkPhysicalDevice,VkDevice,PFN_vkGetInstanceProcAddr,PFN_vkGetDeviceProcAddr,NVSDK_NGX_Version,const NVSDK_NGX_Parameter*)=nullptr;
    NVSDK_NGX_Result (NVSDK_CONV *create)(VkDevice,VkCommandBuffer,NVSDK_NGX_Feature,const NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**)=nullptr;
    decltype(&NVSDK_NGX_VULKAN_EvaluateFeature) evaluate=nullptr;
    decltype(&NVSDK_NGX_VULKAN_ReleaseFeature) release=nullptr;
    decltype(&NVSDK_NGX_VULKAN_Shutdown1) shutdown=nullptr;
    decltype(&NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements) instance_extensions=nullptr;
    decltype(&NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements) device_extensions=nullptr;
private:
    template<class T> void bind(T& function, const char* name) {
        function=reinterpret_cast<T>(GetProcAddress(module_,name));
        if(!function && error_.empty()) error_=std::string("nvngx_dlssnr.dll missing export ")+name;
    }
    bool identify_caller() {
        auto* base=reinterpret_cast<unsigned char*>(module_);
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
        const auto& imports=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        const size_t size=nt->OptionalHeader.SizeOfImage;
        if(!imports.VirtualAddress || imports.VirtualAddress>=size || imports.Size>size-imports.VirtualAddress) return false;
        const auto* descriptors=reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base+imports.VirtualAddress);
        for(size_t d=0;d<imports.Size/sizeof(*descriptors) && descriptors[d].Name;++d) {
            const auto& descriptor=descriptors[d];
            if(!descriptor.OriginalFirstThunk || descriptor.OriginalFirstThunk>=size || descriptor.FirstThunk>=size) continue;
            auto* names=reinterpret_cast<const IMAGE_THUNK_DATA*>(base+descriptor.OriginalFirstThunk);
            auto* slots=reinterpret_cast<IMAGE_THUNK_DATA*>(base+descriptor.FirstThunk);
            const size_t count=std::min((size-descriptor.OriginalFirstThunk)/sizeof(*names),(size-descriptor.FirstThunk)/sizeof(*slots));
            for(size_t i=0;i<count && names[i].u1.AddressOfData;++i) {
                const auto rva=names[i].u1.AddressOfData;
                if(IMAGE_SNAP_BY_ORDINAL(rva) || rva>=size || size-rva<=sizeof(WORD)) continue;
                const char* name=reinterpret_cast<const char*>(base+rva+sizeof(WORD));
                if(!std::memchr(name,0,size-rva-sizeof(WORD)) || std::strcmp(name,"GetModuleFileNameW")) continue;
                DWORD protection=0;
                if(!VirtualProtect(&slots[i].u1.Function,sizeof(void*),PAGE_READWRITE,&protection)) return false;
                // This import stays valid for the DLL mapping's lifetime; the replacement lives in the executable.
                InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&slots[i].u1.Function),reinterpret_cast<void*>(&snippet_module_name));
                DWORD previous=0;
                return VirtualProtect(&slots[i].u1.Function,sizeof(void*),protection,&previous)!=0;
            }
        }
        return false;
    }
    HMODULE module_=nullptr;
    std::string error_;
};

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
    ~Parameters() override { std::lock_guard lock(snippet_mutex); NVSDK_NGX_VULKAN_DestroyParameters(parameters_); }
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
        std::lock_guard lock(snippet_mutex);
        ngx_log().open(in.log_file);
        snippet_=std::make_unique<Snippet>(in.runtime_directory);
        if(!snippet_->ready()) { error_=snippet_->error(); return uint32_t(NVSDK_NGX_Result_FAIL_UnableToInitializeFeature); }
        discovery_=std::make_unique<Discovery>(in.runtime_directory,in.log_file.has_parent_path() ? in.log_file.parent_path() : std::filesystem::path());
        discovery_->info.LoggingInfo.MinimumLoggingLevel=in.verbose ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_ON;
        device_=in.device;
        auto result=NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId,NVSDK_NGX_ENGINE_TYPE_CUSTOM,"uc-recomp",discovery_->data.c_str(),in.instance,in.physical,in.device,
            in.instance_proc,in.device_proc,&discovery_->info,NVSDK_NGX_Version_API);
        sdk_started_=NVSDK_NGX_SUCCEED(result);
        if(!sdk_started_) return uint32_t(result);
        NVSDK_NGX_Parameter* parameters=nullptr;
        result=NVSDK_NGX_VULKAN_AllocateParameters(&parameters);
        if(NVSDK_NGX_FAILED(result) || !parameters) return uint32_t(NVSDK_NGX_FAILED(result) ? result : NVSDK_NGX_Result_FAIL_InvalidParameter);
        result=snippet_->init(0,discovery_->data.c_str(),in.instance,in.physical,in.device,in.instance_proc,in.device_proc,NVSDK_NGX_Version_API,parameters);
        NVSDK_NGX_VULKAN_DestroyParameters(parameters);
        started_=NVSDK_NGX_SUCCEED(result);
        if(!started_) error_="direct nvngx_dlssnr.dll init: "+ngx_result_name(uint32_t(result));
        return uint32_t(result);
    }
    uint32_t requirements(NgxRequirement& out) override {
        // Custom snippets can retain the shipping GPU requirement. Like RenoDX, let real feature creation decide support.
        out={}; return uint32_t(NVSDK_NGX_Result_FAIL_NotImplemented);
    }
    std::unique_ptr<NgxParameters> allocate_parameters(uint32_t& result) override {
        std::lock_guard lock(snippet_mutex);
        NVSDK_NGX_Parameter* parameters=nullptr;
        result=uint32_t(NVSDK_NGX_VULKAN_AllocateParameters(&parameters));
        if(NVSDK_NGX_FAILED(NVSDK_NGX_Result(result)) || !parameters) return nullptr;
        return std::make_unique<Parameters>(parameters);
    }
    uint32_t create_feature(VkCommandBuffer command, NgxParameters& parameters, void*& handle) override {
        std::lock_guard lock(snippet_mutex);
        NVSDK_NGX_Handle* created=nullptr;
        const auto result=snippet_->create(device_,command,kFeature,static_cast<Parameters&>(parameters).get(),&created);
        if(NVSDK_NGX_FAILED(result)) error_="direct nvngx_dlssnr.dll create: "+ngx_result_name(uint32_t(result));
        handle=created;
        return uint32_t(result);
    }
    uint32_t evaluate(VkCommandBuffer command, void* handle, NgxParameters& parameters) override {
        std::lock_guard lock(snippet_mutex);
        const auto result=snippet_->evaluate(command,static_cast<NVSDK_NGX_Handle*>(handle),static_cast<Parameters&>(parameters).get(),nullptr);
        if(NVSDK_NGX_FAILED(result)) error_="direct nvngx_dlssnr.dll evaluate: "+ngx_result_name(uint32_t(result));
        return uint32_t(result);
    }
    void release_feature(void* handle) override { std::lock_guard lock(snippet_mutex); snippet_->release(static_cast<NVSDK_NGX_Handle*>(handle)); }
    void shutdown() override {
        std::lock_guard lock(snippet_mutex);
        if(started_) { snippet_->shutdown(device_); started_=false; }
        if(sdk_started_) { NVSDK_NGX_VULKAN_Shutdown1(device_); sdk_started_=false; }
        snippet_.reset();
    }
    std::string diagnostic() const override { return error_.empty() ? ngx_log().informative() : error_; }
    ~Backend() override { shutdown(); }
private:
    std::unique_ptr<Discovery> discovery_;
    std::unique_ptr<Snippet> snippet_;
    std::string error_;
    VkDevice device_=VK_NULL_HANDLE;
    bool started_=false, sdk_started_=false;
};
}
std::unique_ptr<NgxBackend> make_ngx_backend() { return std::make_unique<Backend>(); }
std::vector<std::string> ngx_dlss5_instance_extensions(const std::filesystem::path& runtime_directory) {
    std::lock_guard lock(snippet_mutex);
    Snippet snippet(runtime_directory); if(!snippet.ready()) return {};
    Discovery discovery(runtime_directory);
    uint32_t count=0; VkExtensionProperties* properties=nullptr;
    if(NVSDK_NGX_FAILED(snippet.instance_extensions(&discovery.discovery,&count,&properties))) return {};
    return names(count,properties);
}
std::vector<std::string> ngx_dlss5_device_extensions(VkInstance instance, VkPhysicalDevice physical, const std::filesystem::path& runtime_directory) {
    std::lock_guard lock(snippet_mutex);
    Snippet snippet(runtime_directory); if(!snippet.ready()) return {};
    Discovery discovery(runtime_directory);
    uint32_t count=0; VkExtensionProperties* properties=nullptr;
    if(NVSDK_NGX_FAILED(snippet.device_extensions(instance,physical,&discovery.discovery,&count,&properties))) return {};
    return names(count,properties);
}
}
