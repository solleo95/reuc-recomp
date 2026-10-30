#include "dlss_frame_generation.h"
#include "fsr_frame_generation.h"
#include "ngx_dlss5.h"
#include "streamline.h"
#include "upscaler.h"
#include "upscaler_probe.h"
#include <stdexcept>

// Linux: DLSS (Streamline, NGX) and FSR (the FidelityFX DLLs) are Windows runtimes. These definitions keep the renderer's code unchanged and report every
// vendor upscaler and frame generator as unavailable; SMAA, the sharpening pass and the port's own frame generation work without them.
namespace uc {
namespace {
constexpr const char* kReason = "not available on Linux";
}
struct StreamlineApi {};

std::unique_ptr<Streamline> Streamline::load(const std::filesystem::path&, bool, std::string& reason, bool, RendererBackend) { reason = std::string("NVIDIA Streamline is ") + kReason; return nullptr; }
bool Streamline::set_native_device(void*, std::string& reason) { reason = kReason; return false; }
bool Streamline::native_dlss_supported(const std::array<uint8_t, 8>&, std::string& reason) const { reason = kReason; return false; }
bool Streamline::native_ray_reconstruction_supported(const std::array<uint8_t, 8>&, std::string& reason) const { reason = kReason; return false; }
bool Streamline::native_dlss_g_supported(const std::array<uint8_t, 8>&, std::string& reason) const { reason = kReason; return false; }
bool Streamline::upgrade_native_interface(void**, std::string& reason) const { reason = kReason; return false; }
Streamline::~Streamline() = default;
VkResult Streamline::create_instance(const VkInstanceCreateInfo&, VkInstance&) const { return VK_ERROR_INITIALIZATION_FAILED; }
VkResult Streamline::create_device(VkInstance, VkPhysicalDevice, const VkDeviceCreateInfo&, VkDevice&) const { return VK_ERROR_INITIALIZATION_FAILED; }
VkResult Streamline::enumerate_physical_devices(VkInstance, uint32_t*, VkPhysicalDevice*) const { return VK_ERROR_INITIALIZATION_FAILED; }
VkResult Streamline::create_surface(VkInstance, void*, VkSurfaceKHR&) const { return VK_ERROR_INITIALIZATION_FAILED; }
void Streamline::destroy_surface(VkInstance, VkSurfaceKHR) const {}
bool Streamline::dlss_supported(VkPhysicalDevice, std::string& reason) const { reason = kReason; return false; }
bool Streamline::ray_reconstruction_supported(VkPhysicalDevice, std::string& reason) const { reason = kReason; return false; }
bool Streamline::dlss_g_supported(VkPhysicalDevice, std::string& reason) const { reason = kReason; return false; }
PresentFunctions Streamline::present_functions(VkDevice) const { return {}; }
void Streamline::begin_frame(uint32_t) {}
void Streamline::marker(Marker) const {}
void Streamline::enable_reflex() const {}
void Streamline::shutdown() {}

DlssFrameGeneration::DlssFrameGeneration(const Streamline& streamline, uint32_t multiplier) : streamline_(streamline), multiplier_(multiplier) {}
DlssFrameGeneration::~DlssFrameGeneration() = default;
bool DlssFrameGeneration::set_active(bool, bool) { return false; }
void DlssFrameGeneration::tag_hudless(VkCommandBuffer, const UpscaleImage&) {}
void DlssFrameGeneration::tag_hudless_native(void*, const NativeUpscaleImage&) {}
void DlssFrameGeneration::clear_native_tags() {}
void DlssFrameGeneration::after_present() {}

struct FsrFrameGeneration::Impl {};
FsrFrameGeneration::FsrFrameGeneration(VkDevice, VkPhysicalDevice, const Queues&, const std::filesystem::path&) { throw std::runtime_error(std::string("FSR frame generation is ") + kReason); }
FsrFrameGeneration::~FsrFrameGeneration() = default;
PresentFunctions FsrFrameGeneration::present_functions() { return {}; }
void FsrFrameGeneration::create_context(VkExtent2D, VkExtent2D, VkFormat, float) {}
void FsrFrameGeneration::release_context() {}
bool FsrFrameGeneration::has_context() const { return false; }
void FsrFrameGeneration::begin_frame(bool, const UpscaleImage&, uint64_t) {}
bool FsrFrameGeneration::active() const { return false; }
void FsrFrameGeneration::prepare(const UpscaleInputs&, uint64_t) {}
void FsrFrameGeneration::after_present() {}

std::unique_ptr<Upscaler> make_dlss_upscaler(const Streamline&, UpscaleMode) { throw std::runtime_error(std::string("DLSS is ") + kReason); }
std::unique_ptr<Upscaler> make_fsr_upscaler(VkDevice, VkPhysicalDevice, const std::filesystem::path&) { throw std::runtime_error(std::string("FSR is ") + kReason); }

UpscalerSupport probe_native_upscalers(const std::filesystem::path&, RendererBackend) {
    UpscalerSupport support;   // the DirectX renderers do not exist here
    support.dlss_reason = support.dlss_g_reason = support.fsr_reason = support.fsr_fg_reason = support.dlss5_reason = "DirectX is not available on Linux";
    return support;
}

std::unique_ptr<NgxBackend> make_ngx_backend() { return nullptr; }
std::unique_ptr<NgxBackend> make_native_ngx_backend() { return nullptr; }
std::vector<std::string> ngx_dlss5_instance_extensions(const std::filesystem::path&) { return {}; }
std::vector<std::string> ngx_dlss5_device_extensions(VkInstance, VkPhysicalDevice, const std::filesystem::path&) { return {}; }
}  // namespace uc
