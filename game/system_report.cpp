#include "system_report.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace uc {
namespace {
std::string utf8(std::wstring_view text) {
    const int size=WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    std::string result(size_t(size>0 ? size : 0),'\0');
    if(size>0)WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),result.data(),size,nullptr,nullptr);
    return result;
}
std::wstring registry_text(HKEY root,const wchar_t* key,const wchar_t* name) {
    wchar_t value[256]{};DWORD size=sizeof(value)-sizeof(wchar_t),type=0;
    if(RegGetValueW(root,key,name,RRF_RT_REG_SZ,&type,value,&size)!=ERROR_SUCCESS)return {};
    return value;
}
void windows_edition() {
    const wchar_t* key=L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    DWORD revision=0,size=sizeof(revision);
    RegGetValueW(HKEY_LOCAL_MACHINE,key,L"UBR",RRF_RT_REG_DWORD,nullptr,&revision,&size);
    auto display=registry_text(HKEY_LOCAL_MACHINE,key,L"DisplayVersion");
    if(display.empty())display=registry_text(HKEY_LOCAL_MACHINE,key,L"ReleaseId");
    std::fprintf(stderr,"[system] edition=%s version=%s revision=%lu\n",utf8(registry_text(HKEY_LOCAL_MACHINE,key,L"ProductName")).c_str(),utf8(display).c_str(),revision);
}
void client_version(const std::filesystem::path& exe_dir) {
    std::string version="none (development build)";
    std::error_code ignored;
    if(std::filesystem::is_regular_file(exe_dir/L"release.json",ignored)) {
        version="unreadable release.json";
        try {
            std::ifstream in(exe_dir/L"release.json",std::ios::binary);
            const auto json=nlohmann::json::parse(in);
            version=json.value("version",std::string("release.json without version"));
        } catch(const std::exception&) {}
    }
    std::fprintf(stderr,"[client] version=%s directory=%s\n",version.c_str(),utf8(exe_dir.wstring()).c_str());
}
void disk_space(const std::filesystem::path& exe_dir) {
    ULARGE_INTEGER free{},total{};
    if(GetDiskFreeSpaceExW(exe_dir.c_str(),&free,&total,nullptr))std::fprintf(stderr,"[system] install drive free_GB=%llu total_GB=%llu\n",free.QuadPart>>30,total.QuadPart>>30);
}
void adapters() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {std::fprintf(stderr,"[gpu-adapters] DXGI factory unavailable\n");return;}
    for(UINT index=0;;++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if(factory->EnumAdapters1(index,&adapter)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 desc{};
        if(FAILED(adapter->GetDesc1(&desc)))continue;
        LARGE_INTEGER driver{};   // the user-mode driver version, four 16-bit parts (NVIDIA: the last five digits are the public version, 32.0.15.6114 = 561.14)
        const bool known=SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver));
        char version[48]="unknown";
        if(known)std::snprintf(version,sizeof version,"%u.%u.%u.%u",unsigned(HIWORD(driver.HighPart)),unsigned(LOWORD(driver.HighPart)),unsigned(HIWORD(driver.LowPart)),unsigned(LOWORD(driver.LowPart)));
        std::fprintf(stderr,"[gpu-adapters] #%u %s vendor=%04x device=%04x revision=%u VRAM_MB=%llu shared_MB=%llu driver=%s%s\n",index,utf8(desc.Description).c_str(),desc.VendorId,desc.DeviceId,desc.Revision,
            (unsigned long long)(desc.DedicatedVideoMemory>>20),(unsigned long long)(desc.SharedSystemMemory>>20),version,(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) ? " software" : "");
    }
}
void vulkan_layers() {   // implicit layers (overlays such as OBS, RTSS, ReShade) are a known source of stutter and crashes
    for(HKEY root:{HKEY_LOCAL_MACHINE,HKEY_CURRENT_USER}) {
        HKEY key=nullptr;
        if(RegOpenKeyExW(root,L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers",0,KEY_READ|KEY_WOW64_64KEY,&key)!=ERROR_SUCCESS)continue;
        wchar_t name[1024];
        for(DWORD index=0;;++index) {
            DWORD length=1024;
            if(RegEnumValueW(key,index,name,&length,nullptr,nullptr,nullptr,nullptr)!=ERROR_SUCCESS)break;
            std::fprintf(stderr,"[vulkan-layer] implicit %s %s\n",root==HKEY_LOCAL_MACHINE ? "machine" : "user",utf8(std::wstring_view(name,length)).c_str());
        }
        RegCloseKey(key);
    }
}
}
void log_system_report(const std::filesystem::path& exe_dir) {
    client_version(exe_dir);
    windows_edition();
    disk_space(exe_dir);
    adapters();
    vulkan_layers();
    std::fprintf(stderr,"[command-line] %s\n",utf8(GetCommandLineW()).c_str());
}
}
