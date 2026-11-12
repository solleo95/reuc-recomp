#include "android_vulkan.h"
#include <adrenotools/driver.h>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <stdexcept>

namespace uc {
std::string android_load_vulkan() {
    static std::string loaded;
    if (!loaded.empty()) return loaded;
    const char* driver = std::getenv("UC_VULKAN_DRIVER");
    const char* folder = std::getenv("UC_VULKAN_DRIVER_DIR");   // ends with '/': libadrenotools appends the library's name to it
    const char* hooks = std::getenv("UC_NATIVE_LIB_DIR");
    void* library = nullptr;
    if (driver && *driver && folder && *folder && hooks && *hooks) {
        library = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hooks, folder, driver, nullptr, nullptr);
        if (library) loaded = std::string("custom driver ") + folder + driver;
        else std::fprintf(stderr, "[vulkan] the custom driver %s%s did not load: the system's driver instead\n", folder, driver);
    }
    if (!library) {
        library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        loaded = "system driver";
    }
    if (!library) throw std::runtime_error(std::string("no Vulkan library: ") + dlerror());
    const auto get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
    if (!get_instance_proc_addr) throw std::runtime_error("the Vulkan library has no vkGetInstanceProcAddr");
    volkInitializeCustom(get_instance_proc_addr);   // the library stays loaded for the life of the process
    return loaded;
}
}  // namespace uc
