#pragma once
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/ffx_upscale.h>
struct ID3D12Device;
struct ID3D12Resource;
namespace uc {
// FFX's VK and DX12 headers redeclare global enum names; keep native helpers in their own translation unit.
FfxApiResource native_fsr_resource(ID3D12Resource*,uint32_t state=FFX_API_RESOURCE_STATE_COMPUTE_READ);
ffxReturnCode_t create_native_fsr_context(const ffxFunctions&,ffxContext&,ID3D12Device*,ffxCreateContextDescUpscale&);
}
