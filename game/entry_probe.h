#pragma once
#include "ppc_rt.h"
#include "audio_gap_audit.h"
#include "audio_startup.h"
#include "prompt_hooks.h"
namespace uc {
// Initialize before guest execution; callbacks under baton; inspect stats after join.
struct EntryProbeStats {uint64_t events=0,records=0,invalid=0;};
struct PictureProbeStats {uint64_t calls=0,records=0,invalid=0;};
void entry_probe_init(bool);
EntryProbeStats entry_probe_stats();
void game_event_probe(const ppc::Ctx&);
void picture_entry_probe(const ppc::Ctx&,uint32_t entry);
void layout_entry_probe(const ppc::Ctx&);
PictureProbeStats picture_probe_stats();
}
// Only selected diagnostic entries emit probes; other entries fold away.
#undef UC_ENTER
#ifdef UC_TRACE
#define UC_ENTER(a) auto uc_prompt_scope=uc::prompt_scope<a>(c); auto uc_audio_scope=uc::audio_start_scope<a>(c); do { if constexpr((a)==0x80049D64u)uc::audio_wait_ready(c); ppc::trace_enter(a); uc::gx_audit_enter(a,c); if constexpr((a)==0x80068DD8u)uc::audio_gap_audit_request(c); if constexpr((a)==0x802E3C10u || (a)==0x802E3C94u)uc::audio_gap_audit_wpad(c,a); if constexpr((a)==0x800676A0u)uc::game_event_probe(c); if constexpr((a)==0x8038BC60u || (a)==0x803CFA84u)uc::picture_entry_probe(c,a); if constexpr((a)==0x8038AEB0u)uc::layout_entry_probe(c); } while(0)
#else
#define UC_ENTER(a) auto uc_prompt_scope=uc::prompt_scope<a>(c); auto uc_audio_scope=uc::audio_start_scope<a>(c); do { if constexpr((a)==0x80049D64u)uc::audio_wait_ready(c); if constexpr((a)==0x80068DD8u)uc::audio_gap_audit_request(c); if constexpr((a)==0x802E3C10u || (a)==0x802E3C94u)uc::audio_gap_audit_wpad(c,a); if constexpr((a)==0x800676A0u)uc::game_event_probe(c); if constexpr((a)==0x8038BC60u || (a)==0x803CFA84u)uc::picture_entry_probe(c,a); if constexpr((a)==0x8038AEB0u)uc::layout_entry_probe(c); } while(0)
#endif
