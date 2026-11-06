#include "raw_input_backend.h"
#include "raw_pointer_math.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>
namespace uc {
namespace {
constexpr auto kRegisterEvery=std::chrono::milliseconds(250);   // SDL registers and removes its own Raw Input mouse when relative mode turns on and off; ours is put back
constexpr ULONG_PTR kTouchOrPen=0xFF515700;                     // the extra information of a mouse message Windows made from a touch or a pen (SDL ignores them too)
class RawInputBackend final : public PointerBackend {
public:
    explicit RawInputBackend(SDL_Window* window) : window_(window) {
        hwnd_=HWND(SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
        SDL_SetWindowsMessageHook(&RawInputBackend::hook,this);   // nothing is registered yet: see wants()
    }
    ~RawInputBackend() override {
        SDL_SetWindowsMessageHook(nullptr,nullptr);
        if(registered_) { RAWINPUTDEVICE device{1,2,RIDEV_REMOVE,nullptr}; RegisterRawInputDevices(&device,1,sizeof device); }   // SDL puts its own back the next time relative mode turns on
    }
    void hint(bool sdl_absolute,bool playing) override {
        bool changed;
        { std::lock_guard lock(mutex_); changed=sdl_absolute!=sdl_absolute_ || playing!=playing_; sdl_absolute_=sdl_absolute; playing_=playing; }
        if(changed) reregister();
    }
    void poll(std::vector<PointerEvent>& out,std::vector<uint64_t>& removed) override {
        const auto now=std::chrono::steady_clock::now();
        if(now>=next_register_) { next_register_=now+kRegisterEvery; reregister(); }
        std::lock_guard lock(mutex_);
        out.insert(out.end(),queue_.begin(),queue_.end()); queue_.clear();
        removed.insert(removed.end(),gone_.begin(),gone_.end()); gone_.clear();
    }
    bool capture(uint64_t device,bool on) override {
        {
            std::lock_guard lock(mutex_);
            if(on) captured_.insert(device); else captured_.erase(device);
        }
        reregister();
        return true;
    }
    std::string status() const override { return {}; }
    bool all_devices() const override { std::lock_guard lock(mutex_); return legacy_off_locked(); }
private:
    // Windows allows one Raw Input mouse registration per process and the last caller wins: SDL's own (relative mode: a thread with a window of its own) would be replaced and the
    // first player's mouse would go dead. So the backend registers only while SDL reads the cursor's position (light gun mode: SDL has no registration then) or while a pointer
    // is captured (then every mouse reaches the game through here and SDL's mouse events are ignored). Otherwise the devices are told apart by SDL's own events (CoopSession::sdl_mouse).
    bool wants_locked() const { return sdl_absolute_ || !captured_.empty(); }
    bool legacy_off_locked() const { return !captured_.empty() && playing_; }   // playing_: the window has the pointer. The port's menu needs the cursor; the game's own menus and missions do not (a gun would drag the shared cursor and click for the first player)
    static bool hook(void* self,MSG* message) {
        auto* backend=static_cast<RawInputBackend*>(self);
        if(message->message==WM_INPUT) backend->read_input(HRAWINPUT(message->lParam));
        else if(message->message==WM_INPUT_DEVICE_CHANGE && message->wParam==GIDC_REMOVAL) backend->device_removed(uint64_t(uint32_t(uintptr_t(message->lParam))));
        return true;   // SDL goes on with every message
    }
    void reregister() {
        bool wants,legacy_off;
        { std::lock_guard lock(mutex_); wants=wants_locked(); legacy_off=legacy_off_locked(); }
        if(!wants) { registered_=false; return; }   // no REMOVE: SDL's own registration may have replaced ours, and that one must stay
        RAWINPUTDEVICE device{1,2,DWORD(RIDEV_INPUTSINK|RIDEV_DEVNOTIFY|(legacy_off ? RIDEV_NOLEGACY : 0)),hwnd_};
        registered_=RegisterRawInputDevices(&device,1,sizeof device)!=FALSE;
    }
    void device_removed(uint64_t id) {
        std::lock_guard lock(mutex_);
        for(const auto button:held_[id]) { PointerEvent release; release.kind=PointerEvent::Kind::button; release.device=id; release.button=button; release.down=false; queue_.push_back(release); }
        held_.erase(id); captured_.erase(id); gone_.push_back(id);
    }
    void read_input(HRAWINPUT handle) {
        UINT size=0;
        if(GetRawInputData(handle,RID_INPUT,nullptr,&size,sizeof(RAWINPUTHEADER))!=0 || !size) return;
        std::vector<uint8_t> buffer(size);
        if(GetRawInputData(handle,RID_INPUT,buffer.data(),&size,sizeof(RAWINPUTHEADER))!=size) return;
        const auto* input=reinterpret_cast<const RAWINPUT*>(buffer.data());
        if(input->header.dwType!=RIM_TYPEMOUSE) return;
        const RAWMOUSE& mouse=input->data.mouse;
        if((mouse.ulExtraInformation&0xFFFFFF00)==kTouchOrPen) return;
        const uint64_t id=uint64_t(uint32_t(uintptr_t(input->header.hDevice)));
        std::vector<PointerEvent> events;
        if(mouse.usFlags&MOUSE_MOVE_ABSOLUTE) {
            if(const auto at=absolute_position(mouse)) { PointerEvent event; event.kind=PointerEvent::Kind::absolute; event.device=id; event.x=(*at)[0]; event.y=(*at)[1]; events.push_back(event); }
        } else if(mouse.lLastX!=0 || mouse.lLastY!=0) {
            PointerEvent event; event.kind=PointerEvent::Kind::relative; event.device=id; event.x=float(mouse.lLastX); event.y=float(mouse.lLastY); events.push_back(event);
        }
        if(mouse.usButtonFlags&RI_MOUSE_WHEEL) {
            PointerEvent event; event.kind=PointerEvent::Kind::wheel; event.device=id; event.x=float(SHORT(mouse.usButtonData))/WHEEL_DELTA; events.push_back(event);
        }
        static constexpr struct { USHORT flag; uint8_t button; bool down; } kButtons[]={
            {RI_MOUSE_LEFT_BUTTON_DOWN,1,true},{RI_MOUSE_LEFT_BUTTON_UP,1,false},{RI_MOUSE_RIGHT_BUTTON_DOWN,3,true},{RI_MOUSE_RIGHT_BUTTON_UP,3,false},
            {RI_MOUSE_MIDDLE_BUTTON_DOWN,2,true},{RI_MOUSE_MIDDLE_BUTTON_UP,2,false},{RI_MOUSE_BUTTON_4_DOWN,4,true},{RI_MOUSE_BUTTON_4_UP,4,false},
            {RI_MOUSE_BUTTON_5_DOWN,5,true},{RI_MOUSE_BUTTON_5_UP,5,false}};
        for(const auto& entry:kButtons) if(mouse.usButtonFlags&entry.flag) {
            PointerEvent event; event.kind=PointerEvent::Kind::button; event.device=id; event.button=entry.button; event.down=entry.down; events.push_back(event);
        }
        std::lock_guard lock(mutex_);
        for(const auto& event:events) {
            if(event.kind==PointerEvent::Kind::button) { if(event.down) held_[id].insert(event.button); else held_[id].erase(event.button); }
            queue_.push_back(event);
        }
    }
    std::optional<std::array<float,2>> absolute_position(const RAWMOUSE& mouse) const {
        PixelRect desktop;
        if(mouse.usFlags&MOUSE_VIRTUAL_DESKTOP) desktop={GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),GetSystemMetrics(SM_CXVIRTUALSCREEN),GetSystemMetrics(SM_CYVIRTUALSCREEN)};
        else desktop={0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
        RECT client{}; POINT origin{0,0};
        if(!hwnd_ || !GetClientRect(hwnd_,&client) || !ClientToScreen(hwnd_,&origin)) return std::nullopt;
        return raw_absolute_to_window(mouse.lLastX,mouse.lLastY,desktop,PixelRect{origin.x,origin.y,int(client.right-client.left),int(client.bottom-client.top)});
    }
    SDL_Window* window_; HWND hwnd_=nullptr;
    mutable std::mutex mutex_;
    std::vector<PointerEvent> queue_; std::vector<uint64_t> gone_;
    std::map<uint64_t,std::set<uint8_t>> held_;
    std::set<uint64_t> captured_; bool sdl_absolute_=false,playing_=false;
    bool registered_=false;   // main thread only
    std::chrono::steady_clock::time_point next_register_{};
};
}
std::unique_ptr<PointerBackend> make_raw_input_backend(SDL_Window* window) { return std::make_unique<RawInputBackend>(window); }
}
