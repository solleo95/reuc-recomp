#pragma once
#include "native_upscaler.h"
#include "ngx_dlss5.h"
#include <functional>

namespace uc {
struct NativeDlss5Context {
    ID3D12Device* device=nullptr;
    std::filesystem::path shaders,directory;
    std::function<void()> idle;
    std::function<std::unique_ptr<NgxBackend>()> backend; // Optional GPU fixture stand-in.
};
struct NativeDlss5Frame {
    void* command=nullptr; // Open ID3D12GraphicsCommandList.
    NativeUpscaleImage picture,depth,motion;
    std::array<int32_t,2> origin{};
    RenderExtent size{},scene_size{};
    std::array<float,2> jitter{};
    bool reset=false;
    Dlss5Settings settings;
};
class NativeDlss5Pass {
public:
    struct Work;
    NativeDlss5Pass();
    ~NativeDlss5Pass();
    void configure(const NativeDlss5Context&);
    // Pin the result in the submission slot until its GPU fence retires, even when ran() is false.
    // Picture must support a typed RGBA8/RGBA16F UAV. Every input returns to its declared state.
    // Compute bindings are changed; caller restores its own root signature and descriptor heaps.
    std::shared_ptr<Work> record(const NativeDlss5Frame&);
    const std::string& status() const;
    bool failed() const;
    bool ran() const;
    void settings_changed();
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Real feature creation on an isolated hardware DX12 device. Run before loading Streamline.
bool probe_native_dlss5(const std::filesystem::path& directory,std::string& reason);
}
