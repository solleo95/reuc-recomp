#include "evdev_scan.h"
#include "wiimote_transport.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <linux/input.h>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <set>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
namespace uc {
namespace {
// A Wii Remote on Linux is the kernel's hid-wiimote driver: it already talks to the remote and shows it as input nodes (buttons, accelerometer, IR, nunchuk). The transport turns
// those nodes into the 0x37 reports the shared protocol layer reads, so the pointer, swing and nunchuk code is the Windows code. Output reports that set the remote up are accepted
// and ignored (the driver did that); the LED report lights the player LED through sysfs, the motor bit drives force feedback.
std::mutex g_claims;
constexpr auto kRescanEvery=std::chrono::seconds(1);   // the nunchuk node appears when the extension is plugged in
class EvdevRemote final : public WiiTransport {
public:
    EvdevRemote(std::string unique,std::set<std::string>* claimed) : unique_(std::move(unique)),claimed_(claimed) {}
    ~EvdevRemote() override {
        if(ff_id_>=0) ioctl(button_fd(),EVIOCRMFF,ff_id_);
        for(auto& node:nodes_) ::close(node.fd);
        if(claimed_) { std::lock_guard lock(g_claims); claimed_->erase(unique_); }
    }
    bool has_buttons() const { return button_fd()>=0; }
    // Opens the nodes of this remote that are not open yet. Returns false when an open fails for lack of permission.
    bool attach(std::vector<EvdevNode> mine,bool& denied) {
        for(const auto& node:mine) {
            if(std::any_of(nodes_.begin(),nodes_.end(),[&](const Node& open){return open.index==node.index;})) continue;
            int fd=::open(node.path.c_str(),(node.role==EvdevRole::wiimote_buttons ? O_RDWR : O_RDONLY)|O_NONBLOCK|O_CLOEXEC);   // read-write on the button node: force feedback
            if(fd<0 && node.role==EvdevRole::wiimote_buttons) fd=::open(node.path.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);
            if(fd<0) { if(errno==EACCES || errno==EPERM) denied=true; continue; }
            nodes_.push_back({fd,node.index,node.role});
            if(node.role==EvdevRole::wiimote_buttons) hid_dir_=hid_directory(node.index);
        }
        return !denied;
    }
    std::string id() const override { return unique_; }
    bool read(std::vector<uint8_t>& report,unsigned timeout_ms) override {
        report.clear();
        const auto now=std::chrono::steady_clock::now();
        if(now>=next_scan_) {   // a nunchuk plugged in later
            next_scan_=now+kRescanEvery;
            const auto all=scan_evdev_nodes();
            std::vector<EvdevNode> mine; for(const auto& node:all) if(node.caps.unique==unique_ && node.role>=EvdevRole::wiimote_buttons) mine.push_back(node);
            bool denied=false; attach(std::move(mine),denied);
            for(auto at=nodes_.begin();at!=nodes_.end();) {   // a node that is gone (the nunchuk unplugged) is dropped; the remote itself going is seen on its button node
                const bool present=std::any_of(all.begin(),all.end(),[&](const EvdevNode& node){return node.index==at->index;});
                if(present || at->role==EvdevRole::wiimote_buttons) ++at; else { ::close(at->fd); at=nodes_.erase(at); }
            }
        }
        std::vector<pollfd> fds; for(const auto& node:nodes_) fds.push_back({node.fd,POLLIN,0});
        const int ready=::poll(fds.data(),nfds_t(fds.size()),int(timeout_ms));
        if(ready<0 && errno!=EINTR) return false;
        for(size_t i=0;i<fds.size();++i) {
            if(fds[i].revents&(POLLERR|POLLHUP|POLLNVAL)) { if(nodes_[i].role==EvdevRole::wiimote_buttons) return false; continue; }
            if(!(fds[i].revents&POLLIN)) continue;
            input_event raw[64];
            for(;;) {
                const ssize_t got=::read(nodes_[i].fd,raw,sizeof raw);
                if(got<0) { if(errno==EAGAIN || errno==EINTR) break; if(nodes_[i].role==EvdevRole::wiimote_buttons) return false; break; }   // ENODEV: the remote is gone
                if(got==0) break;
                std::vector<EvdevEvent> events;
                for(size_t k=0;k<size_t(got)/sizeof(input_event);++k) events.push_back({raw[k].type,raw[k].code,raw[k].value});
                feed_wiimote(state_,nodes_[i].role,events);
                if(size_t(got)<sizeof raw) break;
            }
        }
        const bool nunchuk=std::any_of(nodes_.begin(),nodes_.end(),[](const Node& node){return node.role==EvdevRole::wiimote_nunchuk;});
        state_.state.has_nunchuk=nunchuk;
        if(pending_status_) {   // report 0x20: the answer to a status request; bit 1 of the flags is "an extension is plugged in"
            pending_status_=false;
            report={0x20,0,0,uint8_t(nunchuk ? 0x02 : 0),0,0,0xC0};
            return true;
        }
        const auto bytes=wii_encode_report(state_.state);   // a remote reports its state continuously; the nodes only on change, so the latest state goes again each time
        report.assign(bytes.begin(),bytes.end());
        return true;
    }
    bool write(std::span<const uint8_t> report) override {
        if(report.size()<2) return false;
        if(report[0]==0x15) pending_status_=true;                 // status request
        else if(report[0]==0x11) set_leds(report[1]>>4);          // player LEDs
        set_motor((report[1]&1)!=0);                              // bit 0 of the first data byte of every output report is the motor
        return true;
    }
private:
    struct Node { int fd; int index; EvdevRole role; };
    int button_fd() const { for(const auto& node:nodes_) if(node.role==EvdevRole::wiimote_buttons) return node.fd; return -1; }
    // The HID device of a node: /sys/class/input/eventN/device is the input device, two levels up is the HID device whose "leds" directory holds the player LEDs.
    static std::string hid_directory(int index) {
        char resolved[4096];
        const std::string link="/sys/class/input/event"+std::to_string(index)+"/device/../..";
        return ::realpath(link.c_str(),resolved) ? std::string(resolved) : std::string{};
    }
    void set_leds(unsigned bits) {   // best effort: brightness is writable by root and by whoever a udev rule says
        if(hid_dir_.empty()) return;
        DIR* directory=opendir((hid_dir_+"/leds").c_str());
        if(!directory) return;
        while(const dirent* entry=readdir(directory)) {
            const std::string name=entry->d_name;
            const int player=evdev_led_player(name);   // hid-wiimote's LEDs are p0..p3
            if(player<0) continue;
            std::ofstream brightness(hid_dir_+"/leds/"+name+"/brightness");
            if(brightness) brightness<<((bits>>player&1) ? "1" : "0");
        }
        closedir(directory);
    }
    void set_motor(bool on) {
        if(on==motor_ || button_fd()<0) return;
        if(ff_id_<0) {
            ff_effect effect{}; effect.type=FF_RUMBLE; effect.id=-1; effect.u.rumble.strong_magnitude=0xFFFF; effect.u.rumble.weak_magnitude=0xFFFF; effect.replay.length=0;
            if(ioctl(button_fd(),EVIOCSFF,&effect)<0) return;   // no force feedback: the motor stays off
            ff_id_=effect.id;
        }
        input_event play{}; play.type=EV_FF; play.code=uint16_t(ff_id_); play.value=on ? 1 : 0;
        if(::write(button_fd(),&play,sizeof play)==ssize_t(sizeof play)) motor_=on;
    }
    std::string unique_;
    std::set<std::string>* claimed_;
    std::vector<Node> nodes_;
    WiiEvdevState state_;
    std::string hid_dir_;
    std::chrono::steady_clock::time_point next_scan_{};
    bool pending_status_=false,motor_=false;
    int ff_id_=-1;
};
std::unique_ptr<WiiTransport> open_remote(std::set<std::string>* claimed,std::string& error) {
    error.clear();
    std::map<std::string,std::vector<EvdevNode>> remotes;   // the nodes of each remote by its unique id
    for(auto& node:scan_evdev_nodes()) if(node.role>=EvdevRole::wiimote_buttons && !node.caps.unique.empty()) remotes[node.caps.unique].push_back(std::move(node));
    bool denied=false;
    for(auto& [unique,nodes]:remotes) {
        const bool has_buttons=std::any_of(nodes.begin(),nodes.end(),[](const EvdevNode& node){return node.role==EvdevRole::wiimote_buttons;});
        if(!has_buttons) continue;
        if(claimed) { std::lock_guard lock(g_claims); if(!claimed->insert(unique).second) continue; }   // another WiimoteInput has this one
        auto remote=std::make_unique<EvdevRemote>(unique,claimed);   // from here the claim is released by its destructor
        remote->attach(std::move(nodes),denied);
        if(remote->has_buttons()) return remote;
    }
    if(denied) error="a Wii Remote is connected but /dev/input is not readable (add the user to the input group, or add a udev rule)";
    return nullptr;
}
}
std::unique_ptr<WiiTransport> wii_open_first(std::string& error) { return open_remote(nullptr,error); }
std::unique_ptr<WiiTransport> wii_open_unclaimed(std::set<std::string>& claimed,std::string& error) { return open_remote(&claimed,error); }
}
