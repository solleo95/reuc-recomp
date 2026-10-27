#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "dlss_frame_generation.h"
#include "streamline.h"
#include "streamline_api.h"
#include "native_upscaler.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace uc {
namespace {
const sl::ViewportHandle kViewport{0};   // the same viewport DLSS SR tags depth and vectors on
// Streamline generates no frames while its window is not the foreground one ("DLSS-G disabled: window not focused"). With
// UC_FG_UNFOCUSED=1 (set by the test harness) sl.common.dll is told this process's window is always the foreground one: its
// GetForegroundWindow import is redirected, nothing is focused or moved, and the player's own window keeps working as before.
HWND own_window() {
    static std::atomic<HWND> cached{nullptr};   // Streamline's threads and the render thread ask
    if (HWND known=cached.load(); known && IsWindow(known)) return known;
    HWND window=nullptr;
    EnumWindows([](HWND candidate, LPARAM out) -> BOOL {
        DWORD process=0; GetWindowThreadProcessId(candidate, &process);
        if (process!=GetCurrentProcessId() || !IsWindowVisible(candidate) || GetWindow(candidate, GW_OWNER)) return TRUE;
        *reinterpret_cast<HWND*>(out)=candidate; return FALSE;
    }, reinterpret_cast<LPARAM>(&window));
    cached.store(window);
    return window;
}
HWND WINAPI foreground_is_ours() { return own_window(); }
bool redirect_foreground_window(const char* module_name) {
    HMODULE module=GetModuleHandleA(module_name);
    if (!module) return false;
    const auto base=reinterpret_cast<BYTE*>(module);
    const auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    const auto& directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    bool patched=false;
    for (auto* import=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+directory.VirtualAddress); directory.VirtualAddress && import->Name; ++import) {
        auto* names=reinterpret_cast<IMAGE_THUNK_DATA*>(base+(import->OriginalFirstThunk ? import->OriginalFirstThunk : import->FirstThunk));
        auto* table=reinterpret_cast<IMAGE_THUNK_DATA*>(base+import->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++table) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* name=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData);
            if (std::strcmp(name->Name,"GetForegroundWindow")!=0) continue;
            DWORD old=0;
            if (!VirtualProtect(&table->u1.Function,sizeof(void*),PAGE_READWRITE,&old)) continue;
            table->u1.Function=reinterpret_cast<ULONG_PTR>(&foreground_is_ours);
            VirtualProtect(&table->u1.Function,sizeof(void*),old,&old);
            patched=true;
        }
    }
    return patched;
}
}
DlssFrameGeneration::DlssFrameGeneration(const Streamline& streamline, uint32_t multiplier) : streamline_(streamline) {
    streamline_.enable_reflex();
    if (std::getenv("UC_FG_UNFOCUSED")) {   // every Streamline module that imports the query (sl.common.dll today), so a release that moves the check still works
        int patched=0;
        for (const char* module:{"sl.common.dll","sl.dlss_g.dll","sl.pcl.dll","sl.reflex.dll","sl.interposer.dll"}) patched+=redirect_foreground_window(module);
        if (!patched) std::fprintf(stderr,"DLSS Frame Generation: no Streamline module imports the foreground window query; frame generation needs the focused window\n");
    }
    sl::DLSSGState state{};
    uint32_t most=1;
    if (UC_SL_CALL(sl::kFeatureDLSS_G,slDLSSGGetState,kViewport,state,nullptr)==sl::Result::eOk) { state_valid_=true; minimum_dimension_=state.minWidthOrHeight; if(state.numFramesToGenerateMax>=1) most=state.numFramesToGenerateMax; }
    multiplier_=std::clamp<uint32_t>(multiplier,2,most+1);
    if (multiplier_!=multiplier) std::fprintf(stderr,"DLSS Frame Generation: %ux requested, this GPU allows %ux\n",multiplier,multiplier_);
    std::fprintf(stderr,"DLSS Frame Generation: %ux\n",multiplier_);
}
DlssFrameGeneration::~DlssFrameGeneration() {
    if(!configured_) return;
    set_active(false,true);
    streamline_.api().free_resources(sl::kFeatureDLSS_G,kViewport);
}
bool DlssFrameGeneration::set_active(bool active, bool reset) {
    (void)reset;   // the scene change flag travels in the common constants (DLSS SR sets Constants::reset for the same frame)
    if (configured_ && active==active_) return true;
    sl::DLSSGOptions options{};
    options.mode=active ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    options.numFramesToGenerate=multiplier_-1;
    options.flags=sl::DLSSGFlags::eRetainResourcesWhenOff;   // menus and movies come and go: no reallocation stutter
    if (const auto result=UC_SL_CALL(sl::kFeatureDLSS_G,slDLSSGSetOptions,kViewport,options); result!=sl::Result::eOk) {
        std::fprintf(stderr,"DLSS Frame Generation: options failed: %s\n",sl::getResultAsStr(result)); return false;
    }
    active_=active; configured_=true;
    return true;
}
void DlssFrameGeneration::tag_hudless(VkCommandBuffer command, const UpscaleImage& hudless) {
    const auto& api=streamline_.api();
    if (!api.frame) return;
    sl::Resource resource(sl::ResourceType::eTex2d, hudless.image, nullptr, hudless.view, VK_IMAGE_LAYOUT_GENERAL);
    resource.width=hudless.extent.width; resource.height=hudless.extent.height; resource.nativeFormat=uint32_t(hudless.format);
    resource.mipLevels=1; resource.arrayLayers=1; resource.flags=0; resource.usage=hudless.usage;
    const sl::Extent extent{0,0,hudless.extent.width,hudless.extent.height};
    const sl::ResourceTag tag{&resource,sl::kBufferTypeHUDLessColor,sl::ResourceLifecycle::eValidUntilPresent,&extent};
    if (const auto result=api.set_tag_for_frame(*api.frame,kViewport,&tag,1,command); result!=sl::Result::eOk)
        std::fprintf(stderr,"DLSS Frame Generation: hud-less tag failed: %s\n",sl::getResultAsStr(result));
}
void DlssFrameGeneration::after_present() {
    sl::DLSSGState state{};
    state_valid_=UC_SL_CALL(sl::kFeatureDLSS_G,slDLSSGGetState,kViewport,state,nullptr)==sl::Result::eOk;
    if (!state_valid_) return;
    fence_=static_cast<VkSemaphore>(state.inputsProcessingCompletionFence);
    fence_value_=state.lastPresentInputsProcessingCompletionFenceValue;
    presented_+=state.numFramesActuallyPresented; ++rendered_;
    if (state.status!=sl::DLSSGStatus::eOk && uint32_t(state.status)!=last_status_)
        std::fprintf(stderr,"DLSS Frame Generation: status 0x%x (see sl::DLSSGStatus)\n",unsigned(state.status));
    last_status_=uint32_t(state.status);
    if (active_ && rendered_%600==0) std::fprintf(stderr,"DLSS Frame Generation: %.2f presented per rendered frame\n",double(presented_-reported_presented_)/600.0);
    if (rendered_%600==0) reported_presented_=presented_;
}
void DlssFrameGeneration::tag_hudless_native(void* command,const NativeUpscaleImage& hudless) {
    const auto& api=streamline_.api();
    if(!api.frame || !command || !hudless.resource) throw std::invalid_argument("DLSS Frame Generation: missing native frame input");
    sl::Resource resource(sl::ResourceType::eTex2d,hudless.resource,nullptr,nullptr,hudless.state);
    resource.width=hudless.extent.width; resource.height=hudless.extent.height; resource.nativeFormat=hudless.format; resource.mipLevels=resource.arrayLayers=1;
    const sl::Extent extent{0,0,hudless.extent.width,hudless.extent.height};
    const sl::ResourceTag tags[]{
        {&resource,sl::kBufferTypeHUDLessColor,sl::ResourceLifecycle::eValidUntilPresent,&extent},
        {nullptr,sl::kBufferTypeUIColorAndAlpha,sl::ResourceLifecycle::eValidUntilPresent,nullptr}};
    if(const auto result=api.set_tag_for_frame(*api.frame,kViewport,tags,2,command); result!=sl::Result::eOk)
        throw std::runtime_error(std::string("DLSS Frame Generation: native HUD-less tag failed: ")+sl::getResultAsStr(result));
}
void DlssFrameGeneration::clear_native_tags() {
    const auto& api=streamline_.api(); if(!api.frame) return;
    const sl::ResourceTag tags[]{
        {nullptr,sl::kBufferTypeDepth,sl::ResourceLifecycle::eValidUntilPresent,nullptr},
        {nullptr,sl::kBufferTypeMotionVectors,sl::ResourceLifecycle::eValidUntilPresent,nullptr},
        {nullptr,sl::kBufferTypeHUDLessColor,sl::ResourceLifecycle::eValidUntilPresent,nullptr},
        {nullptr,sl::kBufferTypeUIColorAndAlpha,sl::ResourceLifecycle::eValidUntilPresent,nullptr}};
    if(const auto result=api.set_tag_for_frame(*api.frame,kViewport,tags,4,nullptr); result!=sl::Result::eOk)
        throw std::runtime_error(std::string("DLSS Frame Generation: clearing native tags failed: ")+sl::getResultAsStr(result));
}
}
