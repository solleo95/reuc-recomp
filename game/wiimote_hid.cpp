#include "wiimote_transport.h"
#include "wiimote_protocol.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bluetoothapis.h>
#include <cwchar>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <algorithm>
#include <mutex>
#include <string>
namespace uc {
namespace {
std::mutex g_claims;   // the claimed sets of wii_open_unclaimed: two WiimoteInput threads open and close remotes at the same time
std::string utf8(const wchar_t* text) {
    const int size=WideCharToMultiByte(CP_UTF8,0,text,-1,nullptr,0,nullptr,nullptr);
    std::string out(size>0 ? size-1 : 0,'\0');
    if(size>1) WideCharToMultiByte(CP_UTF8,0,text,-1,out.data(),size,nullptr,nullptr);
    return out;
}
// One Wii Remote as a Windows HID device. Reads are overlapped and stay pending between calls; a write goes through WriteFile, and once that is refused (the Microsoft
// Bluetooth stack does for some remotes) through HidD_SetOutputReport, which Dolphin also falls back to.
class HidRemote final : public WiiTransport {
public:
    HidRemote(HANDLE file,size_t input_bytes,size_t output_bytes,std::string id,std::set<std::string>* claimed) : file_(file),input_(input_bytes),output_bytes_(output_bytes),id_(std::move(id)),claimed_(claimed) {
        read_event_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        write_event_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    }
    ~HidRemote() override {
        CancelIoEx(file_,nullptr);
        if(pending_) { DWORD got=0; GetOverlappedResult(file_,&read_,&got,TRUE); }   // the kernel must be done with the buffer before it goes
        CloseHandle(file_);
        if(read_event_) CloseHandle(read_event_);
        if(write_event_) CloseHandle(write_event_);
        if(claimed_) { std::lock_guard lock(g_claims); claimed_->erase(id_); }
    }
    std::string id() const override { return id_; }
    bool read(std::vector<uint8_t>& report,unsigned timeout_ms) override {
        report.clear();
        if(!read_event_) return false;
        if(!pending_) {
            read_={}; read_.hEvent=read_event_; ResetEvent(read_event_);
            if(!ReadFile(file_,input_.data(),DWORD(input_.size()),nullptr,&read_) && GetLastError()!=ERROR_IO_PENDING) return false;
            pending_=true;
        }
        const DWORD waited=WaitForSingleObject(read_event_,timeout_ms);
        if(waited==WAIT_TIMEOUT) return true;
        if(waited!=WAIT_OBJECT_0) return false;
        pending_=false;
        DWORD got=0;
        if(!GetOverlappedResult(file_,&read_,&got,FALSE)) return false;
        report.assign(input_.begin(),input_.begin()+got);
        return true;
    }
    bool write(std::span<const uint8_t> report) override {
        if(report.empty() || report.size()>output_bytes_ || !write_event_) return false;
        std::vector<uint8_t> padded(output_bytes_,0);
        std::copy(report.begin(),report.end(),padded.begin());
        if(!use_output_report_) {
            OVERLAPPED overlapped{}; overlapped.hEvent=write_event_; ResetEvent(write_event_);
            BOOL ok=WriteFile(file_,padded.data(),DWORD(padded.size()),nullptr,&overlapped);
            DWORD written=0;
            if(!ok && GetLastError()==ERROR_IO_PENDING) {
                if(WaitForSingleObject(write_event_,1000)!=WAIT_OBJECT_0) { CancelIoEx(file_,&overlapped); GetOverlappedResult(file_,&overlapped,&written,TRUE); return false; }   // the kernel is done with `overlapped` before it goes
                ok=TRUE;
            }
            if(ok && GetOverlappedResult(file_,&overlapped,&written,FALSE)) return true;
            use_output_report_=true;
        }
        return HidD_SetOutputReport(file_,padded.data(),ULONG(padded.size()))!=FALSE;
    }
private:
    HANDLE file_,read_event_=nullptr,write_event_=nullptr;
    std::vector<uint8_t> input_;
    size_t output_bytes_;
    std::string id_;
    std::set<std::string>* claimed_;
    OVERLAPPED read_{};
    bool pending_=false,use_output_report_=false;
};
struct DeviceList {
    HDEVINFO handle;
    ~DeviceList() { if(handle!=INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(handle); }
};
bool is_wii_remote(const wchar_t* path) {
    HANDLE probe=CreateFileW(path,0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);   // identity needs no access: other devices are not opened for reading
    if(probe==INVALID_HANDLE_VALUE) return false;
    HIDD_ATTRIBUTES attributes{}; attributes.Size=sizeof attributes;
    const bool ours=HidD_GetAttributes(probe,&attributes) && attributes.VendorID==kWiimoteVendor && (attributes.ProductID==kWiimoteProduct || attributes.ProductID==kWiimotePlusProduct);
    CloseHandle(probe);
    return ours;
}
}
namespace {
std::unique_ptr<WiiTransport> open_remote(std::set<std::string>* claimed,std::string& error) {
    error.clear();
    GUID guid; HidD_GetHidGuid(&guid);
    DeviceList devices{SetupDiGetClassDevsW(&guid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)};
    if(devices.handle==INVALID_HANDLE_VALUE) return nullptr;
    bool found=false; DWORD why=0;
    for(DWORD index=0;;++index) {
        SP_DEVICE_INTERFACE_DATA data{}; data.cbSize=sizeof data;
        if(!SetupDiEnumDeviceInterfaces(devices.handle,nullptr,&guid,index,&data)) break;
        DWORD size=0;
        SetupDiGetDeviceInterfaceDetailW(devices.handle,&data,nullptr,0,&size,nullptr);
        if(size<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<uint8_t> storage(size);
        auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data()); detail->cbSize=sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if(!SetupDiGetDeviceInterfaceDetailW(devices.handle,&data,detail,size,nullptr,nullptr) || !is_wii_remote(detail->DevicePath)) continue;
        const std::string id=utf8(detail->DevicePath);
        if(claimed) { std::lock_guard lock(g_claims); if(claimed->count(id)) continue; }   // another WiimoteInput has this one
        found=true;
        HANDLE file=CreateFileW(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        if(file==INVALID_HANDLE_VALUE) { why=GetLastError(); continue; }
        PHIDP_PREPARSED_DATA preparsed=nullptr; HIDP_CAPS caps{};
        const bool sized=HidD_GetPreparsedData(file,&preparsed) && HidP_GetCaps(preparsed,&caps)==HIDP_STATUS_SUCCESS;
        if(preparsed) HidD_FreePreparsedData(preparsed);
        if(!sized || caps.InputReportByteLength<kWiiInputBytes || caps.OutputReportByteLength<2) { CloseHandle(file); continue; }
        bool taken=false;
        if(claimed) { std::lock_guard lock(g_claims); taken=!claimed->insert(id).second; }   // claimed between the check above and now: it is the other one's
        if(taken) { CloseHandle(file); continue; }
        auto remote=std::make_unique<HidRemote>(file,caps.InputReportByteLength,caps.OutputReportByteLength,id,claimed);   // from here the claim is released by its destructor
        const auto request=wii_status_request();
        if(!remote->write(request)) { why=GetLastError(); continue; }   // a bar's empty slot or a remote already gone: look at the next device
        return remote;
    }
    if(found) error="a Wii Remote is paired but did not answer (it may be switched off, or another program has it open; Windows error "+std::to_string(why)+")";
    return nullptr;
}
}
std::unique_ptr<WiiTransport> wii_open_first(std::string& error) { return open_remote(nullptr,error); }
std::unique_ptr<WiiTransport> wii_open_unclaimed(std::set<std::string>& claimed,std::string& error) { return open_remote(&claimed,error); }
std::string wii_sync_devices() {
    BLUETOOTH_FIND_RADIO_PARAMS radio_params{};
    radio_params.dwSize=sizeof(radio_params);
    HANDLE raw_radio=nullptr;
    HBLUETOOTH_RADIO_FIND radios=BluetoothFindFirstRadio(&radio_params,&raw_radio);
    if(!radios) return "No Bluetooth radio found. Connect a Bluetooth adapter or a DolphinBar in Mode 4.";

    bool found_remote=false,enabled_hid=false,authentication_failed=false;
    struct RadioHandle {
        HANDLE value;
        ~RadioHandle() { if(value) CloseHandle(value); }
    };
    do {
        RadioHandle radio{raw_radio};
        BLUETOOTH_RADIO_INFO radio_info{};
        radio_info.dwSize=sizeof(radio_info);
        if(BluetoothGetRadioInfo(radio.value,&radio_info)!=ERROR_SUCCESS) continue;

        BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
        search.dwSize=sizeof(search);
        search.hRadio=radio.value;
        search.fReturnAuthenticated=search.fReturnRemembered=search.fReturnUnknown=TRUE;
        search.fIssueInquiry=TRUE;
        search.cTimeoutMultiplier=2;
        BLUETOOTH_DEVICE_INFO device{};
        device.dwSize=sizeof(device);
        HBLUETOOTH_DEVICE_FIND devices=BluetoothFindFirstDevice(&search,&device);
        if(!devices) continue;
        do {
            if(wcscmp(device.szName,L"Nintendo RVL-CNT-01") &&
               wcscmp(device.szName,L"Nintendo RVL-CNT-01-TR")) continue;
            found_remote=true;
            if(device.fConnected) { enabled_hid=true; continue; }

            bool authenticated=device.fAuthenticated!=FALSE;
            if(!authenticated) {
                WCHAR pin[7]{};
                // Nintendo's red SYNC button uses the host Bluetooth address as its PIN.
                for(int i=0;i<6;++i) pin[i]=WCHAR(radio_info.address.rgBytes[i]);
                DWORD result=BluetoothAuthenticateDevice(nullptr,radio.value,&device,pin,6);
                if(result!=ERROR_SUCCESS) {
                    // The 1+2 guest-pairing flow uses the remote's Bluetooth address instead.
                    for(int i=0;i<6;++i) pin[i]=WCHAR(device.Address.rgBytes[i]);
                    result=BluetoothAuthenticateDevice(nullptr,radio.value,&device,pin,6);
                }
                authenticated=result==ERROR_SUCCESS;
            }
            if(!authenticated) { authentication_failed=true; continue; }

            const GUID hid_service{0x00001124,0,0x1000,{0x80,0,0,0x80,0x5f,0x9b,0x34,0xfb}};
            if(BluetoothSetServiceState(radio.value,&device,&hid_service,BLUETOOTH_SERVICE_ENABLE)==ERROR_SUCCESS)
                enabled_hid=true;
        } while(BluetoothFindNextDevice(devices,&device));
        BluetoothFindDeviceClose(devices);
    } while(BluetoothFindNextRadio(radios,&raw_radio));
    BluetoothFindRadioClose(radios);

    if(!found_remote) return "No Wii Remote discovered. Press the red SYNC button or hold 1+2, then try again.";
    if(enabled_hid) return "Wii Remote discovered; Windows is waiting for its HID connection.";
    if(authentication_failed) return "Wii Remote found, but Bluetooth pairing failed. Close other Bluetooth tools and try again.";
    return "Wii Remote found, but Windows could not enable its HID service.";
}
}
