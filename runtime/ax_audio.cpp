#include "ax_audio.h"
#include "audio_gap_audit.h"
#include "gx_vertices.h"
#include "guest_call.h"
#include "clock.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include "compat.h"   // _mm_prefetch (xmmintrin.h on x86, the ARM prefetch on Apple Silicon)
namespace {
uc::Sched* scheduler;
uc::AudioSink output;
bool started=false;
bool replaying=false;
bool profile_audio=false;
struct VoiceProbe { bool running=false,tracked=false; int32_t peak=0; };
std::array<VoiceProbe,128> voice_probes{};
uint32_t voice_probe_starts=0;
std::atomic<uint64_t> frames{0},nonzero{0};
gx::Memory ram(const ppc::Ctx& c) { return {{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}}; }
int16_t next_sample(ppc::Ctx& c,uint32_t pb) {
    if(ppc::rd16(c,pb+0x10)!=1) return 0;
    uint32_t pos=ppc::rd32(c,pb+0x7A);
    const uint32_t end=ppc::rd32(c,pb+0x76);
    const uint16_t format=ppc::rd16(c,pb+0x70);
    int32_t value;
    if (format==10) {
        if (pos>UINT32_MAX/2) throw std::invalid_argument("AX PCM16 address overflows");
        ram(c).read(pos*2,2); value=int16_t(ppc::rd16(c,pos*2));
    } else if (format==25) {
        ram(c).read(pos,1); value=int8_t(ppc::rd8(c,pos))*256;
    } else {
        if ((pos&15)<2) pos=(pos&~15u)+2;
        const auto data=ram(c).read(pos/2,1);
        if ((pos&15)==2) { ram(c).read((pos&~15u)/2,1); ppc::wr16(c,pb+0xA0,ppc::rd8(c,(pos&~15u)/2)); }
        const auto pred=ppc::rd16(c,pb+0xA0);
        if (pred>255 || (pred>>4)>7) throw std::invalid_argument("invalid AX ADPCM predictor");
        const uint8_t byte=std::to_integer<uint8_t>(data[0]);
        int32_t nibble=(pos&1) ? byte&15 : byte>>4;
        if (nibble>=8) nibble-=16;
        const int32_t first=int16_t(ppc::rd16(c,pb+0x7E + (pred>>4)*4));
        const int32_t second=int16_t(ppc::rd16(c,pb+0x80+(pred>>4)*4));
        const int64_t prediction=int64_t(first)*int16_t(ppc::rd16(c,pb+0xA2))+int64_t(second)*int16_t(ppc::rd16(c,pb+0xA4));
        value=int32_t(std::clamp<int64_t>((int64_t(nibble)*(1<<(pred&15))*2048+prediction+1024)>>11,-32768,32767));
        ppc::wr16(c,pb+0xA4,ppc::rd16(c,pb+0xA2)); ppc::wr16(c,pb+0xA2,uint16_t(value));
    }
    // End is a playback boundary, not an upper RAM bound; streaming may jump beyond it.
    if(pos==end) {
        pos=ppc::rd32(c,pb+0x72);
        if(!ppc::rd16(c,pb+0x6E)) ppc::wr16(c,pb+0x10,0);
        else {
            ppc::wr16(c,pb+0xA0,ppc::rd16(c,pb+0xB4));
            if(ppc::rd16(c,pb+0x12)!=1) {
                ppc::wr16(c,pb+0xA2,ppc::rd16(c,pb+0xB6));
                ppc::wr16(c,pb+0xA4,ppc::rd16(c,pb+0xB8));
            }
        }
    } else ++pos;
    ppc::wr32(c,pb+0x7A,pos);
    return int16_t(value);
}
void arm(uint64_t at) {
    scheduler->post_at(at,[at] {
        if (!started) return;
        hle_AXFrame(scheduler->irq_ctx());
        // Every interval produces96 samples; skipping overdue intervals starves the device.
        arm(at+uc::ms_to_ticks(3));
    },uc::Sched::EventKind::Audio);
}
}
namespace uc {
void mix_ax_voice_reference(ppc::Ctx& c,uint32_t pb,std::span<int32_t,192> mix) {
    ram(c).read(pb,320);
    if (ppc::rd16(c,pb+0x10)!=1) return;
    const auto format=ppc::rd16(c,pb+0x70);
    if (format!=0 && format!=10 && format!=25) throw std::invalid_argument("unsupported native AX sample format");
    const auto ratio=ppc::rd32(c,pb+0xA6);
    if (ratio>4*65536) throw std::invalid_argument("native AX sample rate exceeds4x");
    // shortcut: nearest native resampling;4-tap filtering, AUX/reverb and surround fidelity remain TD-042.
    uint32_t phase=ppc::rd16(c,pb+0xAA);
    int32_t sample=int16_t(ppc::rd16(c,pb+0xB2));
    int32_t envelope=ppc::rd16(c,pb+0x6A);
    int32_t left=ppc::rd16(c,pb+0x14),right=ppc::rd16(c,pb+0x18);
    const auto control=ppc::rd32(c,pb+0x0C);
    for (size_t i=0;i<96;++i) {
        if (ppc::rd16(c,pb+0x10)!=1) break;
        phase+=ratio;
        while (phase>=65536) { sample=next_sample(c,pb); phase-=65536; }
        const auto scaled=int64_t(sample)*envelope/32768;
        if (control&5) mix[i*2]+=int32_t(scaled*left/32768);
        if (control&6) mix[i*2+1]+=int32_t(scaled*right/32768);
        envelope=std::clamp(envelope+int16_t(ppc::rd16(c,pb+0x6C)),0,65535);
        if (control&4) left=std::clamp(left+int16_t(ppc::rd16(c,pb+0x16)),0,65535);
        if (control&4) right=std::clamp(right+int16_t(ppc::rd16(c,pb+0x1A)),0,65535);
    }
    ppc::wr16(c,pb+0xAA,uint16_t(phase)); ppc::wr16(c,pb+0xB2,uint16_t(sample));
    ppc::wr16(c,pb+0x6A,uint16_t(envelope)); ppc::wr16(c,pb+0x14,uint16_t(left)); ppc::wr16(c,pb+0x18,uint16_t(right));
}
namespace {
uint16_t be16(const uint8_t* p) { uint16_t v; std::memcpy(&v,p,2); return _byteswap_ushort(v); }
// The same arithmetic as mix_ax_voice_reference with the voice's state (position, predictor, history, running) in locals and one store of each at the end, instead of a
// load/store and a bounds check of the guest memory per sample. It takes a voice only when nothing it touches can differ from the reference: every sample byte it can
// read in this call (positions from the current one and from the loop start, at most 4 per output sample) lies in valid RAM of one address alias, outside the voice block
// and outside MMIO, and no position overflows. Anything else (and the order of the exceptions) is left to the reference. Returns false without touching anything then.
bool mix_ax_voice_fast(ppc::Ctx& c,uint32_t pb,std::span<int32_t,192> mix) {
    const uint16_t format=uint16_t(ppc::rd16(c,pb+0x70));
    if (format!=0 && format!=10 && format!=25) return false;
    const uint32_t ratio=ppc::rd32(c,pb+0xA6);
    if (ratio>4*65536) return false;
    const bool loops=ppc::rd16(c,pb+0x6E)!=0;
    const uint32_t first=ppc::rd32(c,pb+0x7A),loop_start=ppc::rd32(c,pb+0x72),end=ppc::rd32(c,pb+0x76);
    constexpr uint32_t kSteps=96*4+16;   // next_sample calls of one frame at 4x, and the ADPCM header skip
    const uint32_t low=loops ? std::min(first,loop_start) : first,high=std::max(first,loops ? loop_start : first);
    if (high>0x7FFFFFFFu-kSteps) return false;
    uint32_t low_byte,high_byte;
    if (format==10) { low_byte=low*2; high_byte=(high+kSteps)*2+1; }
    else if (format==25) { low_byte=low; high_byte=high+kSteps; }
    else { low_byte=(low&~15u)/2; high_byte=(high+kSteps)/2; }
    if ((low_byte>>28)!=(high_byte>>28)) return false;
    try { ram(c).read(low_byte,size_t(high_byte-low_byte)+1); } catch (...) { return false; }
    const uint32_t sample_low=low_byte&0x3FFFFFFFu,sample_high=high_byte&0x3FFFFFFFu,block=pb&0x3FFFFFFFu;
    if (sample_high>=block && sample_low<block+320) return false;
    const uint32_t control=ppc::rd32(c,pb+0x0C);
    uint32_t phase=ppc::rd16(c,pb+0xAA);
    int32_t sample=int16_t(ppc::rd16(c,pb+0xB2));
    int32_t envelope=ppc::rd16(c,pb+0x6A);
    int32_t left=ppc::rd16(c,pb+0x14),right=ppc::rd16(c,pb+0x18);
    const int32_t envelope_step=int16_t(ppc::rd16(c,pb+0x6C)),left_step=int16_t(ppc::rd16(c,pb+0x16)),right_step=int16_t(ppc::rd16(c,pb+0x1A));
    const uint8_t* const memory=c.mem;
    const uint8_t* const coefficients=memory+pb+0x7E;
    const bool stream=ppc::rd16(c,pb+0x12)==1;
    const uint16_t loop_predictor=uint16_t(ppc::rd16(c,pb+0xB4)),loop_first=uint16_t(ppc::rd16(c,pb+0xB6)),loop_second=uint16_t(ppc::rd16(c,pb+0xB8));
    uint32_t position=first;
    uint32_t predictor=ppc::rd16(c,pb+0xA0);
    uint16_t history1=uint16_t(ppc::rd16(c,pb+0xA2)),history2=uint16_t(ppc::rd16(c,pb+0xA4));
    bool running=true,wrote_position=false,wrote_predictor=false,wrote_history=false,wrote_running=false;
    const auto store=[&] {   // what the reference has written to the block by now
        if (wrote_position) ppc::wr32(c,pb+0x7A,position);
        if (wrote_predictor) ppc::wr16(c,pb+0xA0,predictor);
        if (wrote_history) { ppc::wr16(c,pb+0xA4,history2); ppc::wr16(c,pb+0xA2,history1); }
        if (wrote_running) ppc::wr16(c,pb+0x10,0);
    };
    const auto next=[&](auto kind) -> int32_t {   // next_sample; kind is the format as a constant, so each format gets a loop of its own
        constexpr int format=decltype(kind)::value;
        if (!running) return 0;
        uint32_t at=position;
        int32_t value;
        if constexpr (format==10) value=int16_t(be16(memory+at*2));
        else if constexpr (format==25) value=int8_t(memory[at])*256;
        else {
            if ((at&15)<2) at=(at&~15u)+2;
            const uint8_t byte=memory[at/2];
            uint32_t pred=predictor;
            if ((at&15)==2) { pred=memory[(at&~15u)/2]; predictor=pred; wrote_predictor=true; }
            if (pred>255 || (pred>>4)>7) { store(); throw std::invalid_argument("invalid AX ADPCM predictor"); }
            int32_t nibble=(at&1) ? byte&15 : byte>>4;
            if (nibble>=8) nibble-=16;
            const uint8_t* const pair=coefficients+(pred>>4)*4;
            const int32_t one=int16_t(be16(pair)),two=int16_t(be16(pair+2));
            const int64_t prediction=int64_t(one)*int16_t(history1)+int64_t(two)*int16_t(history2);
            value=int32_t(std::clamp<int64_t>((int64_t(nibble)*(1<<(pred&15))*2048+prediction+1024)>>11,-32768,32767));
            history2=history1; history1=uint16_t(value); wrote_history=true;
        }
        if (at==end) {
            at=loop_start;
            if (!loops) { running=false; wrote_running=true; }
            else {
                predictor=loop_predictor; wrote_predictor=true;
                if (!stream) { history1=loop_first; history2=loop_second; wrote_history=true; }
            }
        } else ++at;
        position=at; wrote_position=true;
        return int16_t(value);
    };
    // control bit 0 or 2: left output, bit 1 or 2: right output, bit 2: the volumes ramp (and then both outputs are on)
    const auto loop=[&](auto kind,auto left_on,auto right_on,auto ramp_on) {
        for (size_t i=0;i<96;++i) {
            if (!running) break;
            phase+=ratio;
            while (phase>=65536) { sample=next(kind); phase-=65536; }
            const auto scaled=int64_t(sample)*envelope/32768;
            if constexpr (decltype(left_on)::value) mix[i*2]+=int32_t(scaled*left/32768);
            if constexpr (decltype(right_on)::value) mix[i*2+1]+=int32_t(scaled*right/32768);
            envelope=std::clamp(envelope+envelope_step,0,65535);
            if constexpr (decltype(ramp_on)::value) { left=std::clamp(left+left_step,0,65535); right=std::clamp(right+right_step,0,65535); }
        }
    };
    const auto by_control=[&](auto kind) {
        using on=std::true_type; using off=std::false_type;
        switch (control&7) {
        case 0: loop(kind,off{},off{},off{}); break;
        case 1: loop(kind,on{},off{},off{}); break;
        case 2: loop(kind,off{},on{},off{}); break;
        case 3: loop(kind,on{},on{},off{}); break;
        default: loop(kind,on{},on{},on{}); break;
        }
    };
    if (format==10) by_control(std::integral_constant<int,10>{});
    else if (format==25) by_control(std::integral_constant<int,25>{});
    else by_control(std::integral_constant<int,0>{});
    store();
    ppc::wr16(c,pb+0xAA,uint16_t(phase)); ppc::wr16(c,pb+0xB2,uint16_t(sample));
    ppc::wr16(c,pb+0x6A,uint16_t(envelope)); ppc::wr16(c,pb+0x14,uint16_t(left)); ppc::wr16(c,pb+0x18,uint16_t(right));
    return true;
}
}
void mix_ax_voice(ppc::Ctx& c,uint32_t pb,std::span<int32_t,192> mix) {
    ram(c).read(pb,320);
    if (ppc::rd16(c,pb+0x10)!=1) return;
    if (!mix_ax_voice_fast(c,pb,mix)) mix_ax_voice_reference(c,pb,mix);
}
void audio_hle_init(Sched& s,AudioSink sink,bool profile) {
    scheduler=&s; output=std::move(sink); started=false; replaying=false; frames=nonzero=0;
    profile_audio=profile; voice_probes={}; voice_probe_starts=0;
}
void audio_hle_shutdown() { started=false; output={}; scheduler=nullptr; }
AudioStats audio_stats() { return {frames.load(),nonzero.load()}; }
struct AudioSnapshot {
    bool started;
    std::array<VoiceProbe,128> probes;
    uint32_t starts;
    AudioStats stats;
};
std::shared_ptr<const AudioSnapshot> audio_checkpoint() {
    return std::make_shared<AudioSnapshot>(AudioSnapshot{started,voice_probes,voice_probe_starts,audio_stats()});
}
void audio_restore(const AudioSnapshot& saved) {
    started=saved.started; voice_probes=saved.probes; voice_probe_starts=saved.starts;
    frames=saved.stats.frames; nonzero=saved.stats.nonzero_samples;
}
void audio_set_replaying(bool on) { replaying=on; }
}
void hle_AIStartDMA(ppc::Ctx&) {
    if (scheduler && !started) { started=true; arm(uc::now_ticks()+uc::ms_to_ticks(3)); }
}
void hle_AXFrame(ppc::Ctx& c) {
    // Verified RBUE08 AX service/frame callback and320-byte PBs, without DSP command/mail execution.
    ram(c).read(c.r[13]-25636,4);
    const auto count=ppc::rd32(c,c.r[13]-25524),base=ppc::rd32(c,c.r[13]-25536);
    if (count>128) throw std::invalid_argument("invalid AX voice count");
    if (count) {
        ram(c).read(base,size_t(count)*320);
        uc::call_guest(c,0x80275F34,{0x5F50});
        uc::call_guest(c,0x80273688,{});
    }
    const auto callback=ppc::rd32(c,c.r[13]-25636);
    if (callback) uc::call_guest(c,callback,{});
    std::array<int32_t,192> mix{};
    // Each running voice reads the next bytes of its sample data in mix_ax_voice, and those bytes are usually not in the cache: ask for all of them first so the misses overlap
    // instead of stalling one voice after the other. A prefetch changes no state and never faults; the voice array was bounds-checked above.
    for (uint32_t i=0;i<count;++i) {
        const uint8_t* const block=c.mem+base+i*320;
        uint16_t running,format; uint32_t position;
        std::memcpy(&running,block+0x10,2); std::memcpy(&format,block+0x70,2); std::memcpy(&position,block+0x7A,4);
        if (_byteswap_ushort(running)!=1) continue;
        position=_byteswap_ulong(position);
        const uint32_t address=_byteswap_ushort(format)==10 ? position*2 : _byteswap_ushort(format)==25 ? position : position/2;
        _mm_prefetch(reinterpret_cast<const char*>(uintptr_t(c.mem)+address),_MM_HINT_T0);
    }
    const bool audit=uc::audio_gap_audit_enabled();
    for (uint32_t i=0;i<count;++i) {
        const uint32_t pb=base+i*320;
        std::array<int32_t,192> before;
        auto& probe=voice_probes[i];
        if(profile_audio) {
            const bool running=ppc::rd16(c,pb+0x10)==1;
            if(!running && probe.running && probe.tracked) std::fprintf(stderr,"[ax-sfx-end] frame %llu index %u pb %08X whole-voice peak %d\n",(unsigned long long)frames.load(),i,pb,probe.peak);
            if(!running) probe={};
            if(running && !probe.running) {
                probe.tracked=voice_probe_starts<64 && ppc::rd16(c,pb+0x12)==0;
                probe.peak=0;
                if(probe.tracked) {
                    ++voice_probe_starts;
                    std::fprintf(stderr,"[ax-voice] frame %llu index %u pb %08X format %u stream %u control %08X envelope %u LR %u:%u pos %08X end %08X\n",(unsigned long long)frames.load(),i,pb,ppc::rd16(c,pb+0x70),ppc::rd16(c,pb+0x12),ppc::rd32(c,pb+0x0C),ppc::rd16(c,pb+0x6A),ppc::rd16(c,pb+0x14),ppc::rd16(c,pb+0x18),ppc::rd32(c,pb+0x7A),ppc::rd32(c,pb+0x76));
                }
            }
            probe.running=running;
            if(probe.tracked) before=mix;
        }
        uc::AudioVoiceKey audit_key;
        if(audit) { audit_key=uc::audio_gap_voice_key(c,pb); before=mix; }
        uc::mix_ax_voice(c,pb,mix);
        if(audit) uc::audio_gap_audit_voice(audit_key,before,mix);
        if(profile_audio && probe.tracked) {
            for(size_t sample=0;sample<mix.size();++sample) probe.peak=std::max(probe.peak,int32_t(std::abs(mix[sample]-before[sample])));
            if(ppc::rd16(c,pb+0x10)!=1) {
                std::fprintf(stderr,"[ax-sfx-end] frame %llu index %u pb %08X whole-voice peak %d\n",(unsigned long long)frames.load(),i,pb,probe.peak);
                probe={};
            }
        }
    }
    std::array<int16_t,192> pcm{};
    uint64_t active=0;
    for (size_t i=0;i<pcm.size();++i) { pcm[i]=int16_t(std::clamp(mix[i],-32768,32767)); active+=pcm[i]!=0; }
    ++frames; nonzero+=active;
    if (output && !replaying) output(pcm);
}
