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
namespace {
uc::Sched* scheduler;
uc::AudioSink output;
bool started=false;
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
        const int32_t first=int16_t(ppc::rd16(c,pb+0x7E+(pred>>4)*4));
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
    });
}
}
namespace uc {
void mix_ax_voice(ppc::Ctx& c,uint32_t pb,std::span<int32_t,192> mix) {
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
void audio_hle_init(Sched& s,AudioSink sink,bool profile) {
    scheduler=&s; output=std::move(sink); started=false; frames=nonzero=0;
    profile_audio=profile; voice_probes={}; voice_probe_starts=0;
}
void audio_hle_shutdown() { started=false; output={}; scheduler=nullptr; }
AudioStats audio_stats() { return {frames.load(),nonzero.load()}; }
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
    if (output) output(pcm);
}
