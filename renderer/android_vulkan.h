#pragma once
#include <string>
namespace uc {
// Android: loads Vulkan for the renderer through volk (every vk* call of the renderer and of ImGui's backend is one of volk's function pointers).
// UC_VULKAN_DRIVER names a driver library (Turnip, ...) in UC_VULKAN_DRIVER_DIR, the app's private copy; libadrenotools loads it in place of the
// system's through hooks in UC_NATIVE_LIB_DIR (the app's native library folder). Without a driver, or when it does not load, the system's Vulkan.
// Once per process; returns what was loaded, for the log. Throws when there is no Vulkan at all.
std::string android_load_vulkan();
}  // namespace uc
