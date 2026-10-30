#include "system_report.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_4.h>
#include <psapi.h>
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
void hardware() {
    SYSTEM_INFO info{};GetNativeSystemInfo(&info);
    MEMORYSTATUSEX memory{sizeof(memory)};GlobalMemoryStatusEx(&memory);
    std::fprintf(stderr,"[system] cpu=%s logical_processors=%lu ram_MB=%llu\n",utf8(registry_text(HKEY_LOCAL_MACHINE,L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",L"ProcessorNameString")).c_str(),info.dwNumberOfProcessors,memory.ullTotalPhys>>20);
    const auto plan=utf8(registry_text(HKEY_LOCAL_MACHINE,L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes",L"ActivePowerScheme"));
    const char* names[][2]={{"381b4222-f694-41f0-9685-ff5bb260df2e","Balanced"},{"8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c","High performance"},{"a1841308-3541-4fab-bc81-f71556f20b4a","Power saver"},{"e9a42b02-d5df-448d-aa00-03f14749eb61","Ultimate performance"}};
    const char* label="custom";for(const auto& known:names)if(plan==known[0])label=known[1];
    SYSTEM_POWER_STATUS power{};const bool have_power=GetSystemPowerStatus(&power)!=0;
    std::fprintf(stderr,"[system] power_plan=%s (%s) ac=%s battery_percent=%d\n",label,plan.c_str(),!have_power || power.ACLineStatus==255 ? "unknown" : power.ACLineStatus ? "online" : "battery",have_power && power.BatteryLifePercent!=255 ? int(power.BatteryLifePercent) : -1);
    DEVMODEW mode{};mode.dmSize=sizeof(mode);
    if(EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&mode))std::fprintf(stderr,"[system] primary display %lux%lu @ %lu Hz\n",mode.dmPelsWidth,mode.dmPelsHeight,mode.dmDisplayFrequency);
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
    hardware();
    adapters();
    vulkan_layers();
    std::fprintf(stderr,"[command-line] %s\n",utf8(GetCommandLineW()).c_str());
}
void log_runtime_sample() {
    static uint64_t last_tick=0,last_process=0,last_idle=0,last_kernel=0,last_user=0,last_faults=0;
    const auto value=[](FILETIME t){return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
    FILETIME created,exited,kernel,user,idle_time,system_kernel,system_user;
    PROCESS_MEMORY_COUNTERS_EX memory{sizeof(memory)};
    if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user) || !GetSystemTimes(&idle_time,&system_kernel,&system_user) || !K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)))return;
    const uint64_t process=value(kernel)+value(user),system_total=value(system_kernel)+value(system_user),idle=value(idle_time);
    if(last_tick) {
        const double span=double(system_total-last_tick);   // all logical processors together: percent of the whole machine
        std::fprintf(stderr,"[process] working_set_MB=%llu peak_MB=%llu private_MB=%llu page_faults=%llu cpu_percent=%.1f machine_cpu_percent=%.1f\n",memory.WorkingSetSize>>20,memory.PeakWorkingSetSize>>20,memory.PrivateUsage>>20,
            (unsigned long long)(memory.PageFaultCount-last_faults),span>0 ? 100.0*double(process-last_process)/span : 0.0,span>0 ? 100.0*(1.0-double(idle-last_idle)/span) : 0.0);
    }
    last_tick=system_total;last_process=process;last_idle=idle;last_faults=memory.PageFaultCount;
    MEMORYSTATUSEX machine{sizeof(machine)};
    if(GlobalMemoryStatusEx(&machine))std::fprintf(stderr,"[memory] available_MB=%llu total_MB=%llu load_percent=%lu commit_available_MB=%llu\n",machine.ullAvailPhys>>20,machine.ullTotalPhys>>20,machine.dwMemoryLoad,machine.ullAvailPageFile>>20);
    static Microsoft::WRL::ComPtr<IDXGIFactory4> factory;   // video memory this process holds on each GPU against the budget Windows grants it
    if(!factory && FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return;
    for(UINT index=0;;++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;DXGI_ADAPTER_DESC1 desc{};
        if(factory->EnumAdapters1(index,&adapter)==DXGI_ERROR_NOT_FOUND)break;
        if(FAILED(adapter->GetDesc1(&desc)) || (desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) || FAILED(adapter.As(&adapter3)))continue;
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        if(SUCCEEDED(adapter3->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local)) && local.CurrentUsage)
            std::fprintf(stderr,"[vram] #%u used_MB=%llu budget_MB=%llu%s\n",index,local.CurrentUsage>>20,local.Budget>>20,local.CurrentUsage>local.Budget ? " OVER BUDGET" : "");
    }
}
}
