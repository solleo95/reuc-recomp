#include "netplay_game.h"
#include "netplay_saves.h"
#include "netplay_stream.h"
#include "netplay_datagram.h"
#include "game_checkpoint.h"
#include "game_loop.h"
#include "ax_audio.h"
#include "hle/gx.h"
#include "game_input.h"
#include "clock.h"
#include "sched.h"
#include "game_pacing.h"
#include "cpu_sampler.h"
#include "texture_replacement.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <random>
#include <map>
#include <cstdlib>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
namespace uc {
namespace {
using Json=nlohmann::json;
Json parse_packet(std::span<const uint8_t> data) {
    return Json::parse(data.begin(),data.end(),[](int depth,Json::parse_event_t,Json&) {
        if(depth>16)throw std::runtime_error("network message nested too deeply");
        return true;
    });
}
uint64_t integer(const Json& value,uint64_t limit=UINT64_MAX) {
    if(!value.is_number_unsigned() || value.get<uint64_t>()>limit)throw std::runtime_error("invalid network integer");
    return value.get<uint64_t>();
}
void validate_net_frame(const NetFrame& f) {
    const auto scalar=[](float value,float limit) {
        if(!std::isfinite(value) || std::abs(value)>limit)throw std::runtime_error("invalid network coordinate");
    };
    if(f.work_ns>1000000000)throw std::runtime_error("invalid network work estimate");
    const auto& s=f.input.sample; const auto& a=f.input.actions;
    scalar(s.x,1); scalar(s.y,1); scalar(s.dx,2); scalar(s.dy,2);
    if((s.held|s.pressed|s.released)&~0x1C10u)throw std::runtime_error("invalid network buttons");
    constexpr uint64_t mask=(uint64_t(1)<<unsigned(Action::count))-1;
    if((a.held|a.pressed|a.released)&~mask)throw std::runtime_error("invalid network actions");
    if(a.generation!=f.input.generation)throw std::runtime_error("inconsistent network input generation");
    if(a.count>a.events.size())throw std::runtime_error("too many network actions");
    for(size_t i=0;i<a.count;++i) {
        const auto& e=a.events[i];
        if(unsigned(e.action)>=unsigned(Action::count))throw std::runtime_error("invalid network action");
        scalar(e.direction_x,1); scalar(e.direction_y,1);
    }
    if(a.camera[0]<-127 || a.camera[1]<-127)throw std::runtime_error("invalid network camera");
}
std::vector<uint8_t> bytes(const Json& json) {
    const auto text=json.dump();
    if(text.size()>net::max_packet)throw std::runtime_error("network frame too large");
    return {text.begin(),text.end()};
}
uint64_t hash_bytes(uint64_t hash,const uint8_t* data,size_t size) {
    for(size_t i=0;i+8<=size;i+=8) {uint64_t word;std::memcpy(&word,data+i,8);hash=(hash^word)*0x9E3779B97F4A7C15ull;hash^=hash>>29;}
    for(size_t i=size-size%8;i<size;++i)hash=(hash^data[i])*1099511628211ull;
    return hash;
}
uint64_t file_hash(const std::filesystem::path& path,uint64_t hash=0xCBF29CE484222325ull) {
    std::ifstream in(path,std::ios::binary);
    if(!in)throw std::runtime_error("cannot read network compatibility file: "+path.string());
    std::array<uint8_t,65536> data{};
    while(in) {in.read(reinterpret_cast<char*>(data.data()),data.size());hash=hash_bytes(hash,data.data(),size_t(in.gcount()));}
    if(!in.eof())throw std::runtime_error("network compatibility file read failed");
    return hash;
}
uint64_t saves_hash(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> paths;
    if(std::filesystem::exists(root))for(auto it=std::filesystem::recursive_directory_iterator(root);it!=std::filesystem::recursive_directory_iterator();++it) {
        if(it->is_symlink())throw std::runtime_error("network saves cannot contain symlinks");
        if(it->is_directory() && (it->path().filename()==".backups" || it->path().filename()==".deleted"))it.disable_recursion_pending();
        else if(it->is_regular_file())paths.push_back(it->path());
    }
    std::sort(paths.begin(),paths.end());uint64_t hash=0xCBF29CE484222325ull;
    for(const auto& path:paths) {const auto name=path.lexically_relative(root).generic_u8string();hash=hash_bytes(hash,reinterpret_cast<const uint8_t*>(name.data()),name.size());hash=file_hash(path,hash);}
    return hash;
}
}
std::vector<uint8_t> encode_net_frame(const NetFrame& f) {
    validate_net_frame(f);
    const auto& s=f.input.sample;const auto& a=f.input.actions;
    std::vector<uint8_t> wire{'U','C','F',2}; wire.reserve(110+a.count*11);   // version 2 closes the frame with the analog camera (two int8); version 1 frames are refused
    const auto put=[&](uint64_t value,unsigned width) { for(unsigned i=0;i<width;++i)wire.push_back(uint8_t(value>>(i*8))); };
    const auto real=[&](float value) { put(std::bit_cast<uint32_t>(value),4); };
    // Fixed prefix: marker/version, then the uint64 LE input number for transport deduplication.
    put(f.number,8); put(f.checksum,8); put(f.work_ns,8); put(f.qte_epoch,8);
    real(s.x); real(s.y); real(s.dx); real(s.dy);
    put(s.held,4); put(s.pressed,4); put(s.released,4); put(s.valid,1); put(f.input.host,1);
    put(f.input.generation,8);
    put(a.held,8); put(a.pressed,8); put(a.released,8); put(a.generation,8); put(a.count,2);
    for(size_t i=0;i<a.count;++i) {
        const auto& e=a.events[i];
        put(unsigned(e.action),1); put(e.down,1); real(e.direction_x); real(e.direction_y); put(e.pulse,1);
    }
    put(uint8_t(a.camera[0]),1); put(uint8_t(a.camera[1]),1);
    return wire;
}
uint64_t net_work_estimate(std::array<uint64_t,3> recent,uint64_t previous) {
    std::sort(recent.begin(),recent.end());
    // Ignore isolated hitches, react promptly to sustained load, release headroom gradually.
    return std::max(recent[1],(previous*31+recent[1])/32);
}
uint32_t NetTickPolicy::update(uint32_t tick,double step_cost_us) {
    ++seconds_;
    size_t rung=1;
    for(size_t i=0;i<net_ticks.size();++i)if(net_ticks[i]==tick)rung=i;
    // A step that cannot fit the frame at all (over 110%) means the game already runs below real time and the sound stutters: two seconds.
    const double frame_us=1000000.0/tick;
    slow_=step_cost_us>frame_us*1.1 ? slow_+2 : step_cost_us>frame_us*0.95 ? slow_+1 : 0;
    // What the heaviest recent second cost, forgotten with a 70 s half-life: menus and loading are cheap, a fight is not, and a climb that
    // only looked at the last seconds kept taking the rung a fight then dropped again. A single hitch is capped at 30 ms.
    ceiling_=std::max(std::min(step_cost_us,30000.0),ceiling_*0.99);
    if(last_up_ && seconds_-last_up_>300)hold_=base_hold;   // Five calm minutes: forget the backoff.
    if(slow_>=4 && rung>0) {
        slow_=fast_=0;
        if(last_up_ && seconds_-last_up_<120)hold_=std::min(hold_*2,360u);   // The last rise failed: wait twice as long before the next.
        return net_ticks[rung-1];
    }
    if(rung+1>=net_ticks.size()) {fast_=0;return 0;}
    fast_=ceiling_<1000000.0/net_ticks[rung+1]*(net_ticks[rung+1]==60 ? 0.88 : 0.65) ? fast_+1 : 0;   // Would the next tick still leave a third of its frame free, even for the heaviest recent second?
    if(fast_>=hold_) {fast_=0;last_up_=seconds_;return net_ticks[rung+1];}
    return 0;
}
bool net_must_wait(uint64_t cursor,uint64_t remote_next) {
    return cursor+1>remote_next && cursor+1-remote_next>net_prediction_limit;
}
uint32_t net_input_delay(uint64_t max_rtt_ns,uint32_t tick_hz) {
    if(max_rtt_ns>433333333)throw std::invalid_argument("Network RTT exceeds the input buffer budget");
    // Cover normal one-way transit; rollback handles exceptional late arrivals.
    return 2+uint32_t((max_rtt_ns*tick_hz+1999999999)/2000000000);
}
uint64_t net_startup_rtt(std::array<uint64_t,9> samples) {
    // Buffer ordinary jitter; one or two startup retransmissions must not add permanent input lag.
    std::sort(samples.begin(),samples.end());
    return samples[6];
}
uint64_t net_repair_end(uint64_t peer_ack,uint64_t latest) {
    if(latest<4 || peer_ack>latest-4)return peer_ack;
    return peer_ack+std::min<uint64_t>(8,latest-3-peer_ack);
}
std::optional<uint64_t> NetInputClock::sample(uint64_t cursor,uint64_t now_ns,bool replay,uint64_t period) {
    if(cursor>next_-delay_)throw std::logic_error("input sampling skipped a frame");
    head_=std::max(head_,cursor);
    const bool caught_up=cursor==next_-delay_;
    // Replay samples on wall time; forward progress drains that immutable backlog first.
    if(!caught_up && (!replay || now_ns<deadline_))return {};
    // Slots run ahead of the simulation by the pipeline plus a bounded replay lead: a stalled replay cannot queue unbounded input.
    const auto limit=head_+delay_+replay_lead;
    if(!caught_up && next_>limit) {deadline_=now_ns+period;return {};}
    const auto due=caught_up ? 1 : (now_ns-deadline_)/period+1;
    const auto count=std::min(due,limit-next_+1);
    deadline_=caught_up ? now_ns+period : deadline_+count*period;
    if(count<due)deadline_=now_ns+period;
    next_+=count;
    return next_-1;
}
NetFrame predict_net_frame(const NetFrame& previous,uint64_t number) {
    auto predicted=previous; predicted.number=number; predicted.checksum=0;
    auto& sample=predicted.input.sample; sample.dx=sample.dy=0; sample.pressed=sample.released=0;
    auto& actions=predicted.input.actions; actions.pressed=actions.released=0; actions.count=0;
    return predicted;
}
bool same_net_input(const NetFrame& a,const NetFrame& b) {
    const auto& x=a.input; const auto& y=b.input;
    const auto& p=x.sample; const auto& q=y.sample;
    if(x.host!=y.host || x.generation!=y.generation ||
       p.x!=q.x || p.y!=q.y || p.dx!=q.dx || p.dy!=q.dy || p.held!=q.held || p.pressed!=q.pressed || p.released!=q.released || p.valid!=q.valid) return false;
    const auto& u=x.actions; const auto& v=y.actions;
    if(u.held!=v.held || u.pressed!=v.pressed || u.released!=v.released || u.generation!=v.generation || u.count!=v.count || u.camera!=v.camera) return false;
    // The window epoch only changes filtering of shake gestures, not ordinary input.
    const bool different_qte=a.qte_epoch!=b.qte_epoch;
    if(different_qte && ((u.held|u.pressed|u.released)&(uint64_t(1)<<unsigned(Action::qte_shake))))return false;
    for(size_t i=0;i<u.count;++i) {
        const auto& e=u.events[i]; const auto& f=v.events[i];
        if(e.action!=f.action || e.down!=f.down || e.direction_x!=f.direction_x || e.direction_y!=f.direction_y || e.pulse!=f.pulse) return false;
        if(different_qte && e.action==Action::qte_shake)return false;
    }
    return true;
}
void NetQteState::update(uint32_t next_actor,uint32_t next_mode,uint32_t next_target,bool next_active,NativeInput* source) {
    if(actor!=next_actor || mode!=next_mode || target!=next_target || active!=next_active) {
        ++epoch;
        if(source)source->qte_window(false);
    }
    actor=next_actor;mode=next_mode;target=next_target;active=next_active;
    if(source)source->qte_window(active);
}
void NetQteState::filter(NetFrame& frame) const {
    if(active && frame.qte_epoch==epoch)return;
    auto& actions=frame.input.actions;
    const uint64_t mask=~(uint64_t(1)<<unsigned(Action::qte_shake));
    actions.held&=mask;actions.pressed&=mask;actions.released&=mask;
    const auto end=actions.events.begin()+actions.count;
    actions.count=std::remove_if(actions.events.begin(),end,[](auto event){return event.action==Action::qte_shake;})-actions.events.begin();
}
NetFrame decode_net_frame(std::span<const uint8_t> data) {
    constexpr std::array<uint8_t,4> marker{'U','C','F',2};
    if(data.size()<110 || data.size()>net::max_packet || !std::equal(marker.begin(),marker.end(),data.begin()))throw std::runtime_error("invalid network frame size or version");
    size_t at=marker.size();
    const auto get=[&](unsigned width) {
        if(width>data.size()-at)throw std::runtime_error("truncated network frame");
        uint64_t value=0; for(unsigned i=0;i<width;++i)value|=uint64_t(data[at++])<<(i*8);
        return value;
    };
    const auto flag=[&] { const auto value=get(1); if(value>1)throw std::runtime_error("invalid network flag"); return value!=0; };
    const auto real=[&] { return std::bit_cast<float>(uint32_t(get(4))); };
    NetFrame f; f.number=get(8); f.checksum=get(8); f.work_ns=get(8); f.qte_epoch=get(8);
    auto& s=f.input.sample;
    s.x=real(); s.y=real(); s.dx=real(); s.dy=real();
    s.held=uint32_t(get(4)); s.pressed=uint32_t(get(4)); s.released=uint32_t(get(4)); s.valid=flag();
    f.input.host=flag(); f.input.generation=get(8);
    auto& a=f.input.actions;
    a.held=get(8); a.pressed=get(8); a.released=get(8); a.generation=get(8); a.count=size_t(get(2));
    if(a.count>a.events.size() || data.size()-at!=a.count*11+2)throw std::runtime_error("invalid network action count or frame length");
    for(size_t i=0;i<a.count;++i) {
        auto& e=a.events[i];
        e.action=Action(get(1)); e.down=flag(); e.direction_x=real(); e.direction_y=real(); e.pulse=flag();
    }
    a.camera={int8_t(get(1)),int8_t(get(1))};
    validate_net_frame(f);
    return f;
}
struct NetGame::Impl {
    Options options;
    NativeInput& local;
    NativeInput pads[2];
    NetQteState qte[2];
    NetSaves saves;
    Json hello;
    std::unique_ptr<net::Socket> socket;
    std::stop_source stop;
    std::unique_ptr<net::Stream> stream;
    std::unique_ptr<net::Datagram> datagram;
    Sched* scheduler=nullptr;
    SaveFs* files=nullptr;
    std::unique_ptr<RollbackMemory> memory;
    uint64_t tick;                  // Simulation steps per second: agreed in the hello message, changed only through `tick_changes`.
    std::atomic<uint32_t> tick_now{0};   // The same for other threads (present pacing).
    static constexpr uint64_t hash_interval=60,history_limit=120;   // Frames, whatever the tick: both peers hash and prune on the same frames.
    std::map<uint64_t,uint32_t> tick_changes;   // Step -> tick that starts there; applied again by every replay through that step.
    NetTickPolicy tick_policy;
    const bool auto_tick=[]{const char* text=std::getenv("UC_NET_TICK_AUTO");return !(text && text[0]=='0');}();   // UC_NET_TICK_AUTO=0 pins the tick (tests that measure speed)
    // UC_NET_CLIENT_STEP_US=<us>: the joining player's every step takes that much longer (tests of a weak PC).
    const unsigned extra_step_us=[&]{const char* text=std::getenv("UC_NET_CLIENT_STEP_US");return text && !options.netplay->host ? unsigned(std::min(50000L,std::max(0L,std::strtol(text,nullptr,10)))) : 0u;}();
    double cost_us=0,peer_cost_us=0;            // Smoothed wall time one simulated step takes here and on the other PC.
    std::chrono::steady_clock::time_point last_forward{},next_policy{};
    std::chrono::steady_clock::duration idle{};   // Waiting (lead limit, pacing sleep) since the last forward step: not step cost.
    uint64_t forward_tick=0,simulated_ns=0;       // Tick of the latest forward step; game time it has run (replays do not rewind it).
    size_t input_delay=8;
    std::optional<NetInputClock> input_clock;
    uint64_t local_qte_epoch=0;
    struct Frame {
        NetFrame local,remote;
        uint64_t sequence=0;
        std::vector<uint8_t> wire;
        std::shared_ptr<const GameCheckpoint> checkpoint;
        uint64_t state_hash=0;
        std::array<NetQteState,2> qte;
        bool simulated=false;
    };
    std::map<uint64_t,Frame> history;
    std::map<uint64_t,NetFrame> received;
    std::map<uint64_t,uint64_t> remote_hashes;
    std::optional<uint64_t> sent_hash;
    uint64_t cursor=0,head=0,remote_next=0,peer_ack=0,verified=0;
    uint64_t rollback_count=0,replayed_frames=0,max_rollback=0,start_tick=0;
    std::optional<uint64_t> correction;
    bool started=false,logged_initial=false;
    std::atomic<uint64_t> consumed_sequence{0};
    std::atomic<bool> cancelled{false};
    mutable std::mutex mutex;
    std::string failure;
    std::atomic<uint64_t> frame{0};
    std::atomic<uint64_t> simulation_ns{0};
    const bool profile=std::getenv("UC_ROLLBACK_PROFILE")!=nullptr;
    std::chrono::steady_clock::time_point replay_started{};
    std::chrono::steady_clock::time_point step_started{};
    uint64_t step_cycles=0,step_wait=0,step_wake=0,step_switches=0;
    bool step_replay=false;
    std::atomic<bool> waiting{false};
    std::chrono::steady_clock::time_point began{};
    Impl(const Options& o,NativeInput& source,const std::filesystem::path& executable,const std::filesystem::path& root):options(o),local(source),saves(o.netplay->host,root),tick(o.net_tick),tick_now(o.net_tick) {
        hello={{"game","UC-NET-13"},{"build",file_hash(executable)},{"dol",file_hash(o.game/"DATA/sys/main.dol")},{"speed",o.speed},{"god",o.god},{"tick",o.net_tick}};
        if(!o.netplay->host)std::fprintf(stderr,"[netplay] temporary saves: %s\n",saves.root().string().c_str());
    }
    std::vector<uint8_t> exchange(std::span<const uint8_t> out) {
        // Send first on host, receive first on client; bounded packets cannot fill both send buffers.
        if(options.netplay->host) {socket->send(out);return socket->receive();}
        auto received=socket->receive();socket->send(out);return received;
    }
    struct WaitScope {
            std::atomic<bool>& flag;
            explicit WaitScope(std::atomic<bool>& value):flag(value) {flag.store(true);}
            ~WaitScope() {flag.store(false);}
    };
    [[noreturn]] void fail(const std::exception& error) {
        if(!cancelled.load()) {
            const auto transport_error=stream ? stream->error() : std::string{};
            std::lock_guard lock(mutex);failure=transport_error.empty() ? error.what() : transport_error;
            std::fprintf(stderr,"[netplay] %s\n",failure.c_str());
        }
        stop.request_stop();
        throw ThreadExit{};
    }
    void prepare() {
        WaitScope waiting_scope(waiting);
        try {
                socket=std::make_unique<net::Socket>(*options.netplay,stop.get_token(),std::chrono::seconds(60));
                const auto data=exchange(bytes(hello));const auto remote=parse_packet(data);
                if(remote!=hello)throw std::runtime_error("Network startup mismatch: use the same build, game and simulation settings");
                saves.transfer(*socket);
                const auto saved=bytes(Json{{"saves",saves_hash(saves.root())}});
                if(exchange(saved)!=saved)throw std::runtime_error("Network save snapshot mismatch");
                std::array<uint8_t,16> session{};
                if(options.netplay->host) {
                    std::random_device random;
                    for(auto& byte:session) byte=uint8_t(random());
                    socket->send(session);
                } else {
                    const auto token=socket->receive();
                    if(token.size()!=session.size())throw std::runtime_error("invalid UDP session token");
                    std::copy(token.begin(),token.end(),session.begin());
                }
                datagram=std::make_unique<net::Datagram>(*options.netplay,session,stop.get_token());
                const std::array<uint8_t,1> probe{0};
                const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(15);
                for(;;) {
                    datagram->send(probe);
                    if(auto reply=datagram->take();reply && *reply==std::vector<uint8_t>{0}) { datagram->send(probe); break; }
                    if(std::chrono::steady_clock::now()>=until)throw std::runtime_error("UDP connection timed out; allow the game port for UDP");
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                stream=std::make_unique<net::Stream>(*socket,stop,std::vector<uint8_t>{0});
                files->begin_rollback();
                std::fprintf(stderr,"[netplay] connected role=P%u\n",options.netplay->host ? 1 : 2);
        } catch(const std::exception& error) {fail(error);}
    }
    void align_start() {
        const auto ping=bytes(Json{{"start","ping"}}),pong=bytes(Json{{"start","pong"}});
        auto start=std::chrono::steady_clock::now();
        uint64_t max_rtt=0,last_rtt=0,buffer_rtt=0;
        if(options.netplay->host) {
            std::array<uint64_t,9> samples{};
            for(auto& sample:samples) {
                start=std::chrono::steady_clock::now();
                stream->enqueue(ping);
                if(stream->wait()!=pong)throw std::runtime_error("invalid network start acknowledgement");
                last_rtt=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
                max_rtt=std::max(max_rtt,last_rtt);
                sample=last_rtt;
            }
            buffer_rtt=net_startup_rtt(samples);
            input_delay=net_input_delay(buffer_rtt,60);   // Sized for the 60 Hz tick whichever tick the session starts or ends with.
            stream->enqueue(bytes(Json{{"start","go"},{"rtt_ns",buffer_rtt},{"max_rtt_ns",max_rtt}}));
            start=std::chrono::steady_clock::now()+std::chrono::nanoseconds(buffer_rtt/2);
        } else {
            for(unsigned i=0;i<9;++i) {
                if(stream->wait()!=ping)throw std::runtime_error("invalid network start request");
                stream->enqueue(pong);
            }
            const auto go=parse_packet(stream->wait());
            if(!go.is_object() || go.size()!=3 || go.at("start")!="go")throw std::runtime_error("invalid network start signal");
            buffer_rtt=integer(go.at("rtt_ns")); max_rtt=integer(go.at("max_rtt_ns"));
            if(buffer_rtt>max_rtt)throw std::runtime_error("invalid startup RTT range");
            input_delay=net_input_delay(buffer_rtt,60);
            start=std::chrono::steady_clock::now();
        }
        std::fprintf(stderr,"[netplay] input pipeline: %zu frames; max RTT %.1f ms; UDP prediction and rollback\n",input_delay,max_rtt/1e6);
        std::fprintf(stderr,"[netplay] input buffer RTT p75 %.1f ms\n",buffer_rtt/1e6);
        // Remove the first exchange's one-way phase skew before filling the input pipeline.
        start+=std::chrono::milliseconds(100);
        while(!stop.stop_requested() && std::chrono::steady_clock::now()<start)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    void accept(std::span<const uint8_t> packet) {
        if(packet.size()==1 && packet[0]==0) return; // Last startup probe may still be in flight.
        if(!packet.empty() && packet.front()=='{') {
            const auto json=parse_packet(packet);
            if(json.is_object() && json.size()==2 && json.contains("tick") && json.contains("at")) {
                const auto next=integer(json.at("tick"),1000),at=integer(json.at("at"));
                if(options.netplay->host || std::find(net_ticks.begin(),net_ticks.end(),next)==net_ticks.end())throw std::runtime_error("invalid tick change");
                if(at<=cursor || at>cursor+1000)throw std::runtime_error("tick change outside the planned window");
                tick_changes[at]=uint32_t(next);
                return;
            }
            if(!json.is_object() || json.size()!=3 || !json.contains("ack") || !json.contains("cost"))throw std::runtime_error("invalid rollback acknowledgement");
            peer_cost_us=double(integer(json.at("cost"),10000000));
            const auto ack=integer(json.at("ack"));
            if(ack>input_clock->next())throw std::runtime_error("rollback acknowledgement exceeds sent history");
            peer_ack=std::max(peer_ack,ack);
            const auto& hash=json.at("hash");
            if(!hash.is_null()) {
                if(!hash.is_array() || hash.size()!=2)throw std::runtime_error("invalid confirmed-state hash");
                const auto number=integer(hash[0]),value=integer(hash[1]);
                if(number>head+history_limit)throw std::runtime_error("confirmed-state hash is too far ahead");
                if(number>=verified) {
                    const auto [found,inserted]=remote_hashes.emplace(number,value);
                    if(!inserted && found->second!=value)throw std::runtime_error("peer changed a confirmed-state hash");
                    if(remote_hashes.size()>8)throw std::runtime_error("too many pending confirmed-state hashes");
                }
            }
            return;
        }
        auto incoming=decode_net_frame(packet);
        if(incoming.number>head+history_limit)throw std::runtime_error("network input is too far ahead");
        if(!history.empty() && incoming.number<history.begin()->first)return;
        const auto [found,inserted]=received.emplace(incoming.number,incoming);
        if(!inserted && !same_net_input(found->second,incoming))throw std::runtime_error("peer changed an input frame");
        if(auto past=history.find(incoming.number);past!=history.end() && past->second.simulated && !same_net_input(past->second.remote,incoming))
            correction=std::min(correction.value_or(incoming.number),incoming.number);
        while(received.contains(remote_next))++remote_next;
    }
    void receive() {
        for(unsigned count=0;count<128;++count) { auto packet=datagram->take(); if(!packet)break; accept(*packet); }
        for(unsigned count=0;count<16;++count) { auto packet=stream->take(); if(!packet)break; accept(*packet); }
    }
    void send_inputs() {
        if(!head)return;
        const auto latest=input_clock->next()-1;
        for(uint64_t number=latest;number+4>latest && number>=input_delay;--number) {
            const auto& wire=history.at(number).wire;
            if(wire.size()<=net::max_datagram_payload)datagram->send(wire);
            if(number==0)break;
        }
        // Repair a burst in parallel instead of waiting one RTT for every missing frame.
        const auto repair_end=net_repair_end(peer_ack,latest);
        for(auto gap=history.lower_bound(peer_ack);gap!=history.end() && gap->first<repair_end;++gap)
            if(!gap->second.wire.empty() && gap->second.wire.size()<=net::max_datagram_payload)
                datagram->send(gap->second.wire);
        const auto confirmed=std::min(cursor,remote_next)/hash_interval*hash_interval;
        Json hash=nullptr;
        if(auto state=history.find(confirmed);state!=history.end() && state->second.checkpoint)
            hash=Json::array({confirmed,state->second.state_hash});
        const auto acknowledgement=bytes(Json{{"ack",remote_next},{"hash",hash},{"cost",uint64_t(std::min(cost_us,1e7))}});
        datagram->send(acknowledgement);
        // A lost checksum interval must not look like an input-history outage.
        if(!hash.is_null() && (!sent_hash || confirmed>*sent_hash)) {
            stream->enqueue(acknowledgement);
            sent_hash=confirmed;
        }
    }
    void confirm() {
        for(auto it=remote_hashes.begin();it!=remote_hashes.end();) {
            const auto number=it->first;
            if(number>cursor || number>remote_next) { ++it;continue; }
            auto state=history.find(number);
            if(state==history.end() || !state->second.checkpoint)throw std::runtime_error("confirmed state fell outside rollback history");
            const auto hash=state->second.state_hash;
            if(hash!=it->second)throw std::runtime_error("Network desync at confirmed frame "+std::to_string(number));
            if(number>verified || !logged_initial) {
                game_checkpoint_commit_saves(*state->second.checkpoint,*files);
                verified=number;logged_initial=true;
                std::fprintf(stderr,"[netplay] frame %llu hash %016llX\n",(unsigned long long)number,(unsigned long long)hash);
                std::fprintf(stderr,"[netplay] rollback count %llu replayed %llu max_depth %llu confirmed %llu head %llu tick %llu step cost %.1f ms (peer %.1f ms)\n",
                    (unsigned long long)rollback_count,(unsigned long long)replayed_frames,(unsigned long long)max_rollback,(unsigned long long)verified,(unsigned long long)head,
                    (unsigned long long)tick,cost_us/1000,peer_cost_us/1000);
            }
            it=remote_hashes.erase(it);
        }
        // Actual remote input cannot change. Retain only hash boundaries once replay is impossible;
        // keeping every confirmed GX snapshot would pin hundreds of obsolete GPU texture copies.
        const auto settled=std::min(cursor,remote_next);
        for(auto it=history.begin();it!=history.end() && it->first<settled;++it)
            if(it->first%hash_interval!=0 || it->first<verified)it->second.checkpoint.reset();
        while(!history.empty() && history.begin()->first+history_limit<head) {
            const auto old=history.begin()->first;
            if(old>=verified || old>=peer_ack || old>=remote_next)
                throw std::runtime_error("Network outage exceeded two seconds of rollback history: oldest="+std::to_string(old)+
                    " verified="+std::to_string(verified)+" peer_ack="+std::to_string(peer_ack)+
                    " remote_next="+std::to_string(remote_next)+" head="+std::to_string(head));
            history.erase(history.begin()); received.erase(old);
        }
    }
    void boundary(ppc::Ctx& c) {
        try {
            const auto boundary_started=std::chrono::steady_clock::now();
            if(profile && step_started!=std::chrono::steady_clock::time_point{}) {
                ULONG64 cycles=0; QueryThreadCycleTime(GetCurrentThread(),&cycles);
                const auto elapsed=std::chrono::duration<double,std::milli>(boundary_started-step_started).count();
                const auto switches=scheduler->switch_stats();
                if(step_replay || elapsed>20)
                    std::fprintf(stderr,"[rollback-step] frame %llu replay %d wall %.3f ms main %.3f Mcycles pacing %.3f ms wake %.3f ms switches %llu\n",
                        (unsigned long long)cursor,int(step_replay),elapsed,double(cycles-step_cycles)/1e6,
                        double(game_pacing_stats().wait_ns-step_wait)/1e6,double(switches.wake_ns-step_wake)/1e6,
                        (unsigned long long)(switches.switches-step_switches));
            }
            if(!started) {
                WaitScope scope(waiting); align_start();
                memory=std::make_unique<RollbackMemory>(c.mem);
                remote_next=peer_ack=input_delay;
                input_clock.emplace(input_delay);
                for(uint64_t number=0;number<input_delay;++number) {
                    history[number].local.number=number;
                    received[number].number=number;
                }
                began=std::chrono::steady_clock::now(); start_tick=det_clock_checkpoint(); started=true;
            } else ++cursor;
            receive();
            if(net_must_wait(cursor,remote_next)) {
                // The peer's input is more than the prediction limit behind: wait for it, keeping our own flowing, and give the
                // time back to the virtual clock so the game slows down instead of racing to catch up afterwards.
                const auto wait_began=std::chrono::steady_clock::now();
                for(unsigned spins=0;net_must_wait(cursor,remote_next);++spins) {
                    if(stop.stop_requested())throw ThreadExit{};
                    if(std::chrono::steady_clock::now()-wait_began>std::chrono::seconds(10))throw std::runtime_error("The other player's game stopped responding");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    if(spins%8==0)send_inputs();
                    receive();
                }
                const auto waited=std::chrono::steady_clock::now()-wait_began;
                began+=waited;idle+=waited;
            }
            if(auto_tick && options.netplay->host && boundary_started>=next_policy && tick_changes.upper_bound(cursor)==tick_changes.end()) {
                next_policy=boundary_started+std::chrono::seconds(1);
                if(const auto wanted=tick_policy.update(uint32_t(tick),std::max(cost_us,peer_cost_us))) {
                    // A second and a half beyond the furthest step either PC can be at, so a retransmitted TCP message still arrives in time.
                    const uint64_t at=std::max<uint64_t>(head,remote_next)+input_delay+tick*3/2;
                    tick_changes[at]=wanted;
                    stream->enqueue(bytes(Json{{"tick",wanted},{"at",at}}));
                    std::fprintf(stderr,"[netplay] tick %llu -> %u Hz at step %llu (step cost here %.1f ms, there %.1f ms)\n",(unsigned long long)tick,wanted,(unsigned long long)at,cost_us/1000,peer_cost_us/1000);
                }
            }
            if(correction && *correction<cursor) {
                const auto number=*correction;
                auto& old=history.at(number);
                if(!old.checkpoint)throw std::runtime_error("missing rollback checkpoint");
                if(number<verified)throw std::runtime_error("late input changed confirmed history");
                max_rollback=std::max(max_rollback,cursor-number); ++rollback_count;
                const auto restore_started=std::chrono::steady_clock::now();
                if(profile && replay_started==std::chrono::steady_clock::time_point{})replay_started=restore_started;
                cpu_sampler_set_replaying(true);
                game_restore(*old.checkpoint,*memory,*scheduler,*files);
                if(profile)std::fprintf(stderr,"[rollback-cost] restore depth %llu %.3f ms\n",(unsigned long long)(cursor-number),std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-restore_started).count());
                std::copy(old.qte.begin(),old.qte.end(),qte); cursor=number;
                for(auto it=history.lower_bound(number);it!=history.end();++it) {it->second.simulated=false;it->second.checkpoint.reset();}
            }
            correction.reset();
            {   // The tick in force at this step, also when a rollback replays steps from before or after a change.
                uint32_t wanted=options.net_tick;
                for(const auto& [at,value]:tick_changes) {if(at>cursor)break;wanted=value;}
                if(wanted!=tick) {
                    tick=wanted;tick_now.store(wanted);
                    game_pacing_set_mode({wanted==60,wanted>60 ? wanted : 0});
                    std::fprintf(stderr,"[netplay] simulation tick %u Hz from step %llu\n",wanted,(unsigned long long)cursor);
                }
            }
            const bool replay=cursor<head;
            if(!replay) {   // Wall time per forward step: guest, replays, checkpoints and hashes; the lead wait and the pacing sleep are not cost.
                if(last_forward!=std::chrono::steady_clock::time_point{}) {
                    const double step_us=std::min(50000.0,std::max(0.0,std::chrono::duration<double,std::micro>(boundary_started-last_forward-idle).count()));
                    cost_us=cost_us>0 ? cost_us*0.95+step_us*0.05 : step_us;
                }
                last_forward=boundary_started;idle={};
                forward_tick=tick;
            }
            cpu_sampler_set_replaying(replay);
            audio_set_replaying(replay);
            gx_set_replaying(replay);
            NativeInput* simulated[]={&pads[0],&pads[1]};
            auto& now=history[cursor];
            // A received input is immutable; only predictions can need a later rewind here.
            if(cursor%hash_interval==0 || !received.contains(cursor))
                now.checkpoint=game_checkpoint(*memory,*scheduler,*files,simulated);
            else {
                scheduler->drain_async();
                now.checkpoint.reset();
            }
            if(cursor%hash_interval==0)now.state_hash=game_checkpoint_hash(*now.checkpoint);
            std::copy(std::begin(qte),std::end(qte),now.qte.begin());
            const auto state=input_probe_snapshot(c); const unsigned index=options.netplay->host ? 0 : 1;
            for(unsigned i=0;i<2;++i) {
                const auto& actor=state.players[i];
                qte[i].update(actor.address,actor.mode_630,actor.qte_target_79C,
                    game_input_context(state,i)==InputContext::qte && (actor.flags_798&2),!replay && i==index ? &local : nullptr);
            }
            if(!replay) {
                simulated_ns+=1000000000/tick;simulation_ns.store(simulated_ns);   // Forward steps at the session tick: 1.0 per wall second is full speed.
                if(profile && replay_started!=std::chrono::steady_clock::time_point{}) {
                    std::fprintf(stderr,"[rollback-cost] correction completed %.3f ms\n",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-replay_started).count());
                    replay_started={};
                }
                local.context(game_input_context(state,index));
                local_qte_epoch=qte[index].epoch;
                head=cursor+1; frame.store(head);
            } else ++replayed_frames;
            // Keep sampling during replay; forward progress drains excess queued slots.
            const auto sample_ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            const auto first_sample=input_clock->next();
            if(profile && !replay && cursor%hash_interval==0)
                std::fprintf(stderr,"[rollback-input] frame %llu sample_lead %llu configured %zu\n",(unsigned long long)cursor,(unsigned long long)(first_sample-cursor),input_delay);
            if(const auto last_sample=input_clock->sample(cursor,sample_ns,replay,1000000000/forward_tick)) {
                if(*last_sample>head+history_limit)throw std::runtime_error("input sampling exceeded rollback history");
                for(auto number=first_sample;number<=*last_sample;++number) {
                    auto& future=history[number];
                    if(number==*last_sample) {
                        future.local.number=number; future.local.qte_epoch=local_qte_epoch;
                        future.local.input=local.capture_frame(); future.sequence=local.last_consumed_sequence();
                    } else {
                        const auto& previous=history.at(number-1);
                        future.local=predict_net_frame(previous.local,number); future.sequence=previous.sequence;
                    }
                    future.wire=encode_net_frame(future.local);
                    if(future.wire.size()>net::max_datagram_payload)stream->enqueue(future.wire);
                    else if(*last_sample-number>=4)datagram->send(future.wire); // Every skipped slot gets its first send now.
                }
            }
            if(auto actual=received.find(cursor);actual!=received.end())now.remote=actual->second;
            else now.remote=predict_net_frame(cursor ? history.at(cursor-1).remote : NetFrame{},cursor);
            auto own=now.local,other=now.remote;
            qte[index].filter(own);qte[1-index].filter(other);
            pads[index].install_frame(own.input);pads[1-index].install_frame(other.input);
            now.simulated=true;
            if(!replay)consumed_sequence.store(now.sequence);
            confirm();send_inputs();
            if(profile) {
                const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-boundary_started).count();
                if(replay || elapsed>2)std::fprintf(stderr,"[rollback-step] boundary %llu replay %d wall %.3f ms\n",(unsigned long long)cursor,int(replay),elapsed);
            }
            if(!replay && options.speed==1) {
                const auto ticks=det_clock_checkpoint()-start_tick;
                const auto ns=(ticks/kTimebaseHz)*1000000000+(ticks%kTimebaseHz)*1000000000/kTimebaseHz;
                const auto target=began+std::chrono::nanoseconds(ns);
                const auto sleep_began=std::chrono::steady_clock::now();
                while(!stop.stop_requested() && std::chrono::steady_clock::now()<target)std::this_thread::sleep_for(std::chrono::milliseconds(1));
                idle+=std::chrono::steady_clock::now()-sleep_began;
            }
            if(extra_step_us)std::this_thread::sleep_for(std::chrono::microseconds(extra_step_us));   // Diagnostic: stands in for a slower CPU.
            if(profile) {
                ULONG64 cycles=0; QueryThreadCycleTime(GetCurrentThread(),&cycles); step_cycles=cycles;
                step_wait=game_pacing_stats().wait_ns;
                const auto switches=scheduler->switch_stats(); step_wake=switches.wake_ns; step_switches=switches.switches;
                step_replay=replay; step_started=std::chrono::steady_clock::now();
            }
        } catch(const std::exception& error) {fail(error);}
    }
};
NetGame::NetGame(const Options& o,NativeInput& input,const std::filesystem::path& exe,const std::filesystem::path& saves):impl_(std::make_unique<Impl>(o,input,exe,saves)) {}
NetGame::~NetGame() {cancel();input_hle_set_poll({});game_loop_set_boundary({});audio_set_replaying(false);gx_set_replaying(false);}
void NetGame::prepare() {impl_->prepare();}
const std::filesystem::path& NetGame::save_root() const {return impl_->saves.root();}
void NetGame::install(Sched& scheduler,SaveFs& saves) {
    impl_->scheduler=&scheduler;impl_->files=&saves;scheduler.set_rollback(true);
    input_hle_init(&impl_->pads[0],&impl_->pads[1]);
    input_hle_set_aim_channel(impl_->options.netplay->host ? 0 : 1);
    game_input_init(&impl_->pads[0],impl_->options.input_probe,&impl_->pads[1]);
    game_loop_set_boundary([this](ppc::Ctx& c){impl_->boundary(c);});
}
void NetGame::cancel() {impl_->cancelled.store(true);impl_->stop.request_stop();}
std::string NetGame::error() const {std::lock_guard lock(impl_->mutex);return impl_->failure;}
bool NetGame::waiting() const {return impl_->waiting.load();}
uint64_t NetGame::frames() const {return impl_->frame.load();}
uint32_t NetGame::tick() const {return impl_->tick_now.load();}
uint64_t NetGame::simulation_ns() const {return impl_->simulation_ns.load();}
uint64_t NetGame::consumed_sequence() const {return impl_->consumed_sequence.load();}
}
