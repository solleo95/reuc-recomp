#include "audio_gap_audit.h"
#include "gx_vertices.h"
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
namespace uc {
namespace {
std::atomic<bool> enabled{false};
std::mutex mutex;
std::map<AudioVoiceKey,AudioGapRow> rows;
uint64_t overflow=0;
AudioSpeakerStats speaker;
std::vector<AudioRequestRow> requests;
uint64_t request_count=0;
std::vector<AudioStartRow> starts;
const char* name(AudioHandling handling) {
    switch(handling) {
    case AudioHandling::Main:return "main";
    case AudioHandling::Inactive:return "inactive";
    case AudioHandling::UnsupportedFormat:return "unsupported-format";
    case AudioHandling::UnsupportedRate:return "unsupported-rate";
    case AudioHandling::NoMainRoute:return "no-main-route";
    }
    return "invalid";
}
}
void audio_gap_audit_enable(bool value) {
    std::lock_guard lock(mutex); rows.clear();overflow=0;speaker={};requests.clear();starts.clear();request_count=0;enabled.store(value);
}
bool audio_gap_audit_enabled() { return enabled.load(std::memory_order_relaxed); }
AudioVoiceKey audio_gap_voice_key(ppc::Ctx& c,uint32_t pb) {
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    memory.read(pb,320);
    AudioVoiceKey key;
    key.format=ppc::rd16(c,pb+0x70); key.loop=ppc::rd16(c,pb+0x6E);
    key.src=ppc::rd16(c,pb+8); key.state=ppc::rd16(c,pb+0x10);
    key.stream=ppc::rd16(c,pb+0x12);key.control=ppc::rd32(c,pb+0x0C);
    if(key.state!=1) key.handling=AudioHandling::Inactive;
    else if(key.format!=0 && key.format!=10 && key.format!=25) key.handling=AudioHandling::UnsupportedFormat;
    else if(ppc::rd32(c,pb+0xA6)>4*65536) key.handling=AudioHandling::UnsupportedRate;
    else if(!(key.control&7)) key.handling=AudioHandling::NoMainRoute;
    else key.handling=AudioHandling::Main;
    return key;
}
void audio_gap_audit_voice(AudioVoiceKey key,std::span<const int32_t,192> before,std::span<const int32_t,192> after) {
    if(!audio_gap_audit_enabled()) return;
    uint64_t peak=0;
    for(size_t i=0;i<after.size();++i) {
        const int64_t delta=int64_t(after[i])-before[i];
        peak=std::max(peak,uint64_t(delta<0?-delta:delta));
    }
    std::lock_guard lock(mutex);
    if(!enabled.load()) return;
    auto it=rows.find(key);
    if(it==rows.end()) {
        if(rows.size()==kAudioAuditMaxRows) { ++overflow;return; }
        it=rows.emplace(key,AudioGapRow{key}).first;
    }
    auto& row=it->second;++row.blocks;row.audible_blocks+=peak!=0;row.peak=std::max(row.peak,peak);
}
std::vector<AudioGapRow> audio_gap_audit_rows() {
    std::lock_guard lock(mutex);std::vector<AudioGapRow> result;
    for(const auto& [key,row]:rows) result.push_back(row);
    return result;
}
void audio_gap_audit_wpad(const ppc::Ctx& c,uint32_t entry) {
    if(!audio_gap_audit_enabled()) return;
    // RBUE08: verified readiness check and stream-send entry, observed without replacing SDK behaviour.
    if(entry!=0x802E3C10 && entry!=0x802E3C94) return;
    std::lock_guard lock(mutex);
    if(!enabled.load()) return;
    if(entry==0x802E3C10) ++speaker.queries;
    else { ++speaker.sends; speaker.bytes+=c.r[5]; }
    if(c.r[3]>3) { ++speaker.invalid;return; }
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    auto read=[&](uint32_t at) {
        const auto b=memory.read(at,4);
        return uint32_t(std::to_integer<uint8_t>(b[0]))<<24 | uint32_t(std::to_integer<uint8_t>(b[1]))<<16 | uint32_t(std::to_integer<uint8_t>(b[2]))<<8 | std::to_integer<uint8_t>(b[3]);
    };
    try {
        const auto device=read(0x8065C490+c.r[3]*4);
        if(!device || device>UINT32_MAX-0x8E0) { ++speaker.invalid;return; }
        speaker.disconnected+=read(device+0x8BC)==UINT32_MAX;
        speaker.disabled+=read(device+0x8DC)==0;
    } catch(const gx::Error&) { ++speaker.invalid; }
}
AudioSpeakerStats audio_gap_speaker_stats() { std::lock_guard lock(mutex);return speaker; }
void audio_gap_audit_request(const ppc::Ctx& c) {
    if(!audio_gap_audit_enabled())return;
    std::lock_guard lock(mutex);
    if(!enabled.load())return;
    ++request_count;
    if(requests.size()>=128)return;
    AudioRequestRow row{c.lr,c.r[3],c.r[4],c.r[5]};
    if(c.mem && row.address) {
        gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
        try {
            for(uint32_t i=0;i<96 && row.address<=UINT32_MAX-i;++i) {
                auto value=std::to_integer<uint8_t>(memory.read(row.address+i,1)[0]);
                if(!value) {row.valid=true;break;}
                if(value<32 || value>126)break;
                row.name.push_back(char(value));
            }
        } catch(const gx::Error&) { row.valid=false; }
    }
    std::fprintf(stderr,"[audio-request] %llu caller %08X address %08X position %08X channel %08X valid %u name %s\n",
        (unsigned long long)request_count,row.caller,row.address,row.position,row.channel,unsigned(row.valid),row.name.c_str());
    requests.push_back(std::move(row));
}
std::vector<AudioRequestRow> audio_gap_requests() {std::lock_guard lock(mutex);return requests;}
void audio_gap_audit_start(AudioStartRow row) {
    if(!audio_gap_audit_enabled())return;
    std::lock_guard lock(mutex);
    if(!enabled.load() || starts.size()>=512)return;
    starts.push_back(row);
    std::fprintf(stderr,"[audio-start] entry %08X caller %08X object %08X argument %08X result %08X\n",row.entry,row.caller,row.object,row.argument,row.result);
}
std::vector<AudioStartRow> audio_gap_starts() {std::lock_guard lock(mutex);return starts;}
void audio_gap_audit_report(std::FILE* out) {
    std::lock_guard lock(mutex);
    std::fprintf(out,"AX native mixer: blocks are 3ms, audible means nonzero contribution before final clipping.\n");
    for(const auto& [key,row]:rows)
        std::fprintf(out,"format %u loop %u src %u state %u stream %u control %08X %s blocks %llu audible %llu peak %llu\n",key.format,key.loop,key.src,key.state,key.stream,key.control,name(key.handling),(unsigned long long)row.blocks,(unsigned long long)row.audible_blocks,(unsigned long long)row.peak);
    std::fprintf(out,"WPAD: readiness queries %llu send attempts %llu requested bytes %llu disconnected %llu speaker disabled %llu invalid %llu\n",(unsigned long long)speaker.queries,(unsigned long long)speaker.sends,(unsigned long long)speaker.bytes,(unsigned long long)speaker.disconnected,(unsigned long long)speaker.disabled,(unsigned long long)speaker.invalid);
    std::fprintf(out,"unrecorded blocks (row cap): %llu\n",(unsigned long long)overflow);
    std::fprintf(out,"Sound requests: %llu, retained %zu (first128; details in process log)\n",(unsigned long long)request_count,requests.size());
}
}
