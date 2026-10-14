#pragma once
#include "ppc_rt.h"
#include <compare>
#include <cstdio>
#include <span>
#include <string>
#include <vector>
namespace uc {
enum class AudioHandling { Main, Inactive, UnsupportedFormat, UnsupportedRate, NoMainRoute };
struct AudioVoiceKey {
    uint16_t format=0,loop=0,src=0,state=0,stream=0;
    uint32_t control=0;
    AudioHandling handling=AudioHandling::Inactive;
    auto operator<=>(const AudioVoiceKey&) const = default;
};
struct AudioGapRow { AudioVoiceKey key; uint64_t blocks=0,audible_blocks=0,peak=0; };
inline constexpr size_t kAudioAuditMaxRows=512;
void audio_gap_audit_enable(bool enabled);
bool audio_gap_audit_enabled();
AudioVoiceKey audio_gap_voice_key(ppc::Ctx& c,uint32_t pb);
void audio_gap_audit_voice(AudioVoiceKey key,std::span<const int32_t,192> before,std::span<const int32_t,192> after);
std::vector<AudioGapRow> audio_gap_audit_rows();
struct AudioSpeakerStats { uint64_t queries=0,sends=0,bytes=0,disconnected=0,disabled=0,invalid=0; };
void audio_gap_audit_wpad(const ppc::Ctx& c,uint32_t entry);
AudioSpeakerStats audio_gap_speaker_stats();
struct AudioRequestRow { uint32_t caller=0,address=0,position=0,channel=0;std::string name;bool valid=false; };
void audio_gap_audit_request(const ppc::Ctx& c);
std::vector<AudioRequestRow> audio_gap_requests();
struct AudioStartRow { uint32_t entry,caller,object,argument,result; };
void audio_gap_audit_start(AudioStartRow row);
std::vector<AudioStartRow> audio_gap_starts();
struct AudioStartScope {
    const ppc::Ctx& c;
    AudioStartRow row;
    ~AudioStartScope() { row.result=c.r[3];audio_gap_audit_start(row); }
};
template<uint32_t Entry> auto audio_start_scope(const ppc::Ctx& c) {
    if constexpr(Entry==0x803A6794u || Entry==0x8004C084u || Entry==0x803A67ACu || Entry==0x803ABAB8u || Entry==0x803A9230u || Entry==0x803A9DECu || Entry==0x803A6A38u || Entry==0x803A6B50u || Entry==0x803A7E5Cu || Entry==0x803A6748u || Entry==0x803A6784u || Entry==0x80049D64u)
        return AudioStartScope{c,{Entry,c.lr,c.r[3],c.r[4],0}};
    else { struct Empty {};return Empty{}; }
}
void audio_gap_audit_report(std::FILE* out);
}
