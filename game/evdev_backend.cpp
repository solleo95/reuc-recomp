#include "evdev_backend.h"
#include "evdev_scan.h"
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <linux/input.h>
#include <map>
#include <set>
#include <sys/ioctl.h>
#include <unistd.h>
namespace uc {
namespace {
constexpr auto kRescanEvery=std::chrono::seconds(2);
class EvdevPointerBackend final : public PointerBackend {
public:
    ~EvdevPointerBackend() override { for(auto& [index,device]:devices_) ::close(device.fd); }   // closing a node lets go of its grab
    void poll(std::vector<PointerEvent>& out,std::vector<uint64_t>& removed) override {
        const auto now=std::chrono::steady_clock::now();
        if(now>=next_scan_) { next_scan_=now+kRescanEvery; rescan(out,removed); }
        for(auto at=devices_.begin();at!=devices_.end();) {
            if(read_device(at->second,out)) { ++at; continue; }
            drop(at->second,out,removed);
            at=devices_.erase(at);
        }
    }
    bool capture(uint64_t id,bool on) override {
        for(auto& [index,device]:devices_) if(device.id==id) return ioctl(device.fd,EVIOCGRAB,on ? 1 : 0)==0;
        return false;
    }
    std::string status() const override {
        return denied_ && devices_.empty() ? "no access to /dev/input (add the user to the input group, or add a udev rule)" : "";
    }
private:
    struct Device { int fd=-1; uint64_t id=0; EvdevCaps caps; PointerState state; std::set<uint8_t> held; };
    void rescan(std::vector<PointerEvent>& out,std::vector<uint64_t>& removed) {
        const auto nodes=scan_evdev_nodes();
        std::set<int> present;
        for(const auto& node:nodes) present.insert(node.index);
        for(auto at=devices_.begin();at!=devices_.end();) {   // a node that left sysfs
            if(present.count(at->first)) { ++at; continue; }
            drop(at->second,out,removed); at=devices_.erase(at);
        }
        bool refused=false;
        for(const auto& node:nodes) {
            if(devices_.count(node.index) || !evdev_pointer_candidate(node.caps)) continue;
            const int fd=::open(node.path.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);
            if(fd<0) { if(errno==EACCES || errno==EPERM) refused=true; continue; }
            Device device; device.fd=fd; device.id=uint64_t(node.index)+1; device.caps=node.caps;
            input_absinfo info{};
            if(device.caps.abs_x && ioctl(fd,EVIOCGABS(ABS_X),&info)==0) { device.caps.abs_min[0]=info.minimum; device.caps.abs_max[0]=info.maximum; device.caps.abs_value[0]=info.value; }
            if(device.caps.abs_y && ioctl(fd,EVIOCGABS(ABS_Y),&info)==0) { device.caps.abs_min[1]=info.minimum; device.caps.abs_max[1]=info.maximum; device.caps.abs_value[1]=info.value; }
            const auto role=classify_evdev(device.caps);
            if(role!=EvdevRole::mouse && role!=EvdevRole::gun) { ::close(fd); continue; }
            devices_.emplace(node.index,std::move(device));
        }
        denied_=refused;
    }
    // False when the device is gone (the node returned an error other than "nothing to read").
    bool read_device(Device& device,std::vector<PointerEvent>& out) {
        input_event raw[64];
        for(;;) {
            const ssize_t got=::read(device.fd,raw,sizeof raw);
            if(got<0) return errno==EAGAIN || errno==EINTR;
            if(got==0) return false;
            std::vector<EvdevEvent> events;
            for(size_t i=0;i<size_t(got)/sizeof(input_event);++i) events.push_back({raw[i].type,raw[i].code,raw[i].value});
            const size_t first=out.size();
            feed_pointer(device.state,device.caps,events,device.id,out);
            for(size_t i=first;i<out.size();++i) if(out[i].kind==PointerEvent::Kind::button) { if(out[i].down) device.held.insert(out[i].button); else device.held.erase(out[i].button); }
            if(size_t(got)<sizeof raw) return true;
        }
    }
    void drop(Device& device,std::vector<PointerEvent>& out,std::vector<uint64_t>& removed) {
        for(const uint8_t button:device.held) { PointerEvent release; release.kind=PointerEvent::Kind::button; release.device=device.id; release.button=button; release.down=false; out.push_back(release); }
        removed.push_back(device.id);
        ::close(device.fd);
    }
    std::map<int,Device> devices_;
    std::chrono::steady_clock::time_point next_scan_{};
    bool denied_=false;
};
}
std::unique_ptr<PointerBackend> make_evdev_pointer_backend() { return std::make_unique<EvdevPointerBackend>(); }
}
