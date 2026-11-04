#pragma once
#include "wiimote_input.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
// A Wii Remote that is not there: the transport the WiimoteInput tests (and the local two-player tests) read from.
struct FakeRemote : uc::WiiTransport {
    std::mutex m;
    std::deque<std::vector<uint8_t>> incoming;
    std::vector<std::vector<uint8_t>> written;
    bool gone=false,refuse_writes=false,silent=false;
    std::string name="remote-1";   // the transport's id
    // A real remote keeps sending its latest state: at first nothing pressed, nothing seen, the accelerometer at rest, no nunchuk; then the last report pushed.
    std::vector<uint8_t> rest{0x37,0,0,0x82,0x82,0x9C,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0,0,0,0,0,0};
    bool read(std::vector<uint8_t>& report,unsigned timeout_ms) override {
        std::unique_lock lock(m);
        if(gone) return false;
        report.clear();
        if(!incoming.empty()) { report=incoming.front(); incoming.pop_front(); return true; }
        const bool quiet=silent; const auto latest=rest;
        lock.unlock(); std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms,5u)));
        if(!quiet) report=latest;
        return true;
    }
    bool write(std::span<const uint8_t> report) override {
        std::lock_guard lock(m);
        if(gone || refuse_writes) return false;
        written.emplace_back(report.begin(),report.end()); return true;
    }
    void push(std::vector<uint8_t> report) { std::lock_guard lock(m); if(report[0]!=0x20) rest=report; incoming.push_back(std::move(report)); }
    size_t writes() { std::lock_guard lock(m); return written.size(); }
    void disconnect() { std::lock_guard lock(m); gone=true; }
};
// A remote the opener hands out once; later calls find nothing (unless more are queued).
struct Opener {
    std::mutex m;
    std::deque<std::shared_ptr<FakeRemote>> remotes;
    unsigned calls=0;
    std::string error;
    uc::WiimoteInput::Opener fn() {
        return [this](std::string& out) -> std::unique_ptr<uc::WiiTransport> {
            std::lock_guard lock(m); ++calls; out=error;
            if(remotes.empty()) return nullptr;
            struct Handle : uc::WiiTransport {
                std::shared_ptr<FakeRemote> remote;
                bool read(std::vector<uint8_t>& r,unsigned t) override { return remote->read(r,t); }
                bool write(std::span<const uint8_t> r) override { return remote->write(r); }
                std::string id() const override { return remote->name; }
            };
            auto handle=std::make_unique<Handle>(); handle->remote=remotes.front(); remotes.pop_front(); return handle;
        };
    }
};
template<class F> static bool wait_for(F ready,int ms=5000) {
    for(int waited=0;waited<ms;waited+=5) { if(ready()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    return ready();
}
// Report 0x37: buttons, accelerometer, basic IR (two dots in the first five bytes), a nunchuk.
static std::vector<uint8_t> report(uint8_t first,uint8_t second,std::array<uint8_t,3> accel={0x82,0x82,0x9C},bool ir=false,bool nunchuk=false,std::array<uint8_t,2> stick={128,128},uint8_t nunchuk_buttons=0xFF) {
    std::vector<uint8_t> r{0x37,first,second,accel[0],accel[1],accel[2]};
    if(ir) r.insert(r.end(),{44,144,0x56,188,154, 0xFF,0xFF,0xFF,0xFF,0xFF}); else r.insert(r.end(),10,0xFF);
    if(nunchuk) r.insert(r.end(),{stick[0],stick[1],0x80,0x80,0xB4,nunchuk_buttons}); else r.insert(r.end(),6,0x00);
    return r;
}
