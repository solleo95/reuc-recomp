#pragma once
#include "renderer_backend.h"
namespace uc {
// Controls the chosen graphics API cannot offer at all: the launcher and the in-game menu hide them (a feature report only greys out what a GPU lacks).
// DirectX 11 has no DXR, the supplied FidelityFX SDK has no D3D11 backend, DLSS/FSR frame generation need D3D12 or Vulkan, the DLSS 5 runtime rejects D3D11.
struct BackendUi { bool ray_tracing, fsr, frame_generation, dlss5; };
constexpr BackendUi backend_ui(RendererBackend backend) {
    const bool full=backend!=RendererBackend::directx11;
    return {full,full,full,full};
}
}
