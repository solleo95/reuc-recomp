#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "ffx_dx12.h"
#include <ffx_api/dx12/ffx_api_dx12.h>
namespace uc {
FfxApiResource native_fsr_resource(ID3D12Resource* resource,uint32_t state) {return ffxApiGetResourceDX12(resource,state);}
ffxReturnCode_t create_native_fsr_context(const ffxFunctions& api,ffxContext& context,ID3D12Device* device,ffxCreateContextDescUpscale& create) {
    ffxCreateBackendDX12Desc backend{};backend.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;backend.device=device;
    create.header.pNext=&backend.header;
    const auto result=api.CreateContext(&context,&create.header,nullptr);create.header.pNext=nullptr;return result;
}
}
