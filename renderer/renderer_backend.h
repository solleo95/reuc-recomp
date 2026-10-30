#pragma once
#include <optional>
#include <string_view>

namespace uc {
enum class RendererBackend { vulkan, directx11, directx12 };
constexpr std::string_view renderer_backend_name(RendererBackend backend) {
    switch (backend) {
    case RendererBackend::vulkan: return "vulkan";
    case RendererBackend::directx11: return "dx11";
    case RendererBackend::directx12: return "dx12";
    }
    return {};
}
constexpr std::optional<RendererBackend> renderer_backend_from_name(std::string_view name) {
    if (name == "vulkan") return RendererBackend::vulkan;
#ifdef _WIN32   // the DirectX backends do not exist on Linux (Steam Deck): "dx11"/"dx12" are unknown renderer names there
    if (name == "dx11") return RendererBackend::directx11;
    if (name == "dx12") return RendererBackend::directx12;
#endif
    return std::nullopt;
}
}
