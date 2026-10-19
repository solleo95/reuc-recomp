#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include "streamline.h"
#include "streamline_api.h"
#include <sl_security.h>
#include <cstdio>
#include <cstdlib>

namespace {
uc::StreamlineApi* g_api=nullptr;   // the loaded Streamline; the sl_*.h feature helpers resolve their functions through it
const bool g_verbose=std::getenv("UC_SL_VERBOSE")!=nullptr;   // diagnostics: every Streamline message
void log_message(sl::LogType type, const char* message) {
    if (type!=sl::LogType::eInfo || g_verbose) std::fprintf(stderr, "Streamline: %s", message);
}
}
// The feature helpers in sl_dlss.h / sl_dlss_g.h / sl_reflex.h call this symbol; Streamline is loaded at run time, not linked.
extern "C" sl::Result slGetFeatureFunction(sl::Feature feature, const char* name, void*& function) {
    if (!g_api || !g_api->get_feature_function) return sl::Result::eErrorNotInitialized;
    return g_api->get_feature_function(feature, name, function);
}

namespace uc {
std::unique_ptr<Streamline> Streamline::load(const std::filesystem::path& directory, bool frame_generation, std::string& reason) {
    const auto interposer=std::filesystem::absolute(directory / "sl.interposer.dll");
    if (!std::filesystem::exists(interposer)) { reason="sl.interposer.dll missing"; return nullptr; }
    if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) { reason="sl.interposer.dll is not signed by NVIDIA"; return nullptr; }
    HMODULE module=LoadLibraryW(interposer.c_str());
    if (!module) { reason="sl.interposer.dll could not be loaded"; return nullptr; }
    std::unique_ptr<Streamline> sl(new Streamline);
    sl->module_=module;
    sl->api_=std::make_unique<StreamlineApi>();
    auto& api=*sl->api_;
    const auto get=[&](auto& target, const char* name) { target=reinterpret_cast<std::remove_reference_t<decltype(target)>>(GetProcAddress(module, name)); return target!=nullptr; };
    if (!get(api.init,"slInit") || !get(api.shutdown,"slShutdown") || !get(api.is_feature_supported,"slIsFeatureSupported") ||
        !get(api.get_feature_function,"slGetFeatureFunction") || !get(api.get_new_frame_token,"slGetNewFrameToken") ||
        !get(api.set_tag_for_frame,"slSetTagForFrame") || !get(api.set_constants,"slSetConstants") || !get(api.evaluate_feature,"slEvaluateFeature") ||
        !get(api.free_resources,"slFreeResources") || !get(api.get_instance_proc,"vkGetInstanceProcAddr") || !get(api.get_device_proc,"vkGetDeviceProcAddr")) {
        reason="sl.interposer.dll lacks an expected export"; return nullptr;
    }
    // DLSS-G needs Reflex and PCL (latency markers); they load only with frame generation.
    std::vector<sl::Feature> features{sl::kFeatureDLSS};
    // Ray Reconstruction loads only when its plugin ships: a requested feature with no DLL must not fail slInit for DLSS.
    if (std::filesystem::exists(directory / "sl.dlss_d.dll")) features.push_back(sl::kFeatureDLSS_RR);
    if (frame_generation) features.insert(features.end(), {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL});
    const std::wstring plugins=std::filesystem::absolute(directory).wstring();
    const wchar_t* paths[]={plugins.c_str()};
    sl::Preferences preferences{};
    preferences.showConsole=false;
    preferences.logLevel=g_verbose ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    preferences.pathsToPlugins=paths; preferences.numPathsToPlugins=1;
    preferences.pathToLogsAndData=nullptr;
    preferences.logMessageCallback=log_message;
    // No over-the-air updates: the shipped DLSS 4.5 DLLs are used as they are (predictable presets, no network access from the game).
    preferences.flags=sl::PreferenceFlags::eDisableCLStateTracking|sl::PreferenceFlags::eUseManualHooking|sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.featuresToLoad=features.data(); preferences.numFeaturesToLoad=uint32_t(features.size());
    preferences.engine=sl::EngineType::eCustom; preferences.engineVersion="uc-recomp";
    preferences.projectId="3f5c9a1e-6b2d-4e8a-9c7f-1d2b3a4c5e6f";
    preferences.renderAPI=sl::RenderAPI::eVulkan;
    g_api=&api;
    if (const auto result=api.init(preferences, sl::kSDKVersion); result!=sl::Result::eOk) {
        g_api=nullptr; reason=std::string("slInit failed: ")+sl::getResultAsStr(result); return nullptr;
    }
    sl->initialized_=true; sl->frame_generation_=frame_generation;
    return sl;
}
Streamline::~Streamline() {
    shutdown();
    if (module_) FreeLibrary(static_cast<HMODULE>(module_));
}
void Streamline::shutdown() {
    if (initialized_) { api_->shutdown(); initialized_=false; }
    if (g_api==api_.get()) g_api=nullptr;
}
VkResult Streamline::create_instance(const VkInstanceCreateInfo& info, VkInstance& instance) const {
    const auto create=reinterpret_cast<PFN_vkCreateInstance>(api_->get_instance_proc(VK_NULL_HANDLE, "vkCreateInstance"));
    return create ? create(&info, nullptr, &instance) : VK_ERROR_INITIALIZATION_FAILED;
}
VkResult Streamline::create_device(VkInstance instance, VkPhysicalDevice physical, const VkDeviceCreateInfo& info, VkDevice& device) const {
    const auto create=reinterpret_cast<PFN_vkCreateDevice>(api_->get_instance_proc(instance, "vkCreateDevice"));
    return create ? create(physical, &info, nullptr, &device) : VK_ERROR_INITIALIZATION_FAILED;
}
VkResult Streamline::enumerate_physical_devices(VkInstance instance, uint32_t* count, VkPhysicalDevice* devices) const {
    const auto enumerate=reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(api_->get_instance_proc(instance, "vkEnumeratePhysicalDevices"));
    return enumerate ? enumerate(instance, count, devices) : VK_ERROR_INITIALIZATION_FAILED;
}
VkResult Streamline::create_surface(VkInstance instance, void* hwnd, VkSurfaceKHR& surface) const {
    const auto create=reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(api_->get_instance_proc(instance, "vkCreateWin32SurfaceKHR"));
    VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    info.hinstance=GetModuleHandleW(nullptr); info.hwnd=static_cast<HWND>(hwnd);
    return create ? create(instance, &info, nullptr, &surface) : VK_ERROR_INITIALIZATION_FAILED;
}
void Streamline::destroy_surface(VkInstance instance, VkSurfaceKHR surface) const {
    if (const auto destroy=reinterpret_cast<PFN_vkDestroySurfaceKHR>(api_->get_instance_proc(instance, "vkDestroySurfaceKHR"))) destroy(instance, surface, nullptr);
}
static bool supported(const StreamlineApi& api, sl::Feature feature, VkPhysicalDevice physical, std::string& reason) {
    sl::AdapterInfo adapter{}; adapter.vkPhysicalDevice=physical;
    const auto result=api.is_feature_supported(feature, adapter);
    if (result==sl::Result::eOk) return true;
    reason=result==sl::Result::eErrorAdapterNotSupported ? "this GPU does not support it" :
           result==sl::Result::eErrorDriverOutOfDate ? "the NVIDIA driver is too old" :
           result==sl::Result::eErrorOSOutOfDate ? "Windows is too old" :
           result==sl::Result::eErrorFeatureMissing ? "its runtime files (sl.*.dll, nvngx_*.dll) are missing" : sl::getResultAsStr(result);
    return false;
}
bool Streamline::dlss_supported(VkPhysicalDevice physical, std::string& reason) const { return supported(*api_, sl::kFeatureDLSS, physical, reason); }
bool Streamline::ray_reconstruction_supported(VkPhysicalDevice physical, std::string& reason) const { return supported(*api_, sl::kFeatureDLSS_RR, physical, reason); }
bool Streamline::dlss_g_supported(VkPhysicalDevice physical, std::string& reason) const {
    if (!frame_generation_) { reason="not requested"; return false; }
    return supported(*api_, sl::kFeatureDLSS_G, physical, reason);
}
void Streamline::begin_frame(uint32_t index) {
    if (!initialized_) return;
    sl::FrameToken* token=nullptr;
    if (api_->get_new_frame_token(token,&index)==sl::Result::eOk) api_->frame=token;
}
void Streamline::marker(Marker marker) const {
    if (!initialized_ || !frame_generation_ || !api_->frame) return;
    static constexpr sl::PCLMarker kMarkers[]={sl::PCLMarker::eSimulationStart,sl::PCLMarker::eSimulationEnd,sl::PCLMarker::eRenderSubmitStart,
        sl::PCLMarker::eRenderSubmitEnd,sl::PCLMarker::ePresentStart,sl::PCLMarker::ePresentEnd};
    UC_SL_CALL(sl::kFeaturePCL,slPCLSetMarker,kMarkers[size_t(marker)],*api_->frame);
}
void Streamline::enable_reflex() const {
    if (!initialized_ || !frame_generation_) return;
    sl::ReflexOptions options{}; options.mode=sl::ReflexMode::eLowLatency;
    if (const auto result=UC_SL_CALL(sl::kFeatureReflex,slReflexSetOptions,options); result!=sl::Result::eOk) std::fprintf(stderr,"Reflex: options failed: %s\n",sl::getResultAsStr(result));
}
PresentFunctions Streamline::present_functions(VkDevice device) const {
    PresentFunctions f;
    const auto proxy=[&]<typename Pfn>(auto& target, const char* name, Pfn) {   // the Pfn argument only names the function type
        if (auto function=api_->get_device_proc(device, name)) target=reinterpret_cast<Pfn>(function);
    };
    proxy(f.create,"vkCreateSwapchainKHR",PFN_vkCreateSwapchainKHR{}); proxy(f.destroy,"vkDestroySwapchainKHR",PFN_vkDestroySwapchainKHR{});
    proxy(f.images,"vkGetSwapchainImagesKHR",PFN_vkGetSwapchainImagesKHR{}); proxy(f.acquire,"vkAcquireNextImageKHR",PFN_vkAcquireNextImageKHR{});
    proxy(f.present,"vkQueuePresentKHR",PFN_vkQueuePresentKHR{}); proxy(f.wait_idle,"vkDeviceWaitIdle",PFN_vkDeviceWaitIdle{});
    return f;
}
}
