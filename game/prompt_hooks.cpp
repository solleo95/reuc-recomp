#include "prompt_hooks.h"
#include "prompt_bridge.h"
#include "game_input.h"
#include "test_state.h"
#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <set>
#include <cstdio>
namespace uc {
namespace {
PromptRegistry registry;
Bindings assignments;   // guest baton owns it; a menu change arrives through `pending`
Bindings pad_assignments=default_pad_bindings();   // the same for the gamepad table, shown while `device` is a gamepad
PromptDevice device;
std::mutex pending_mutex;
std::optional<Bindings> pending;
std::optional<std::pair<Bindings,PromptDevice>> pending_pad;
std::atomic<bool> has_pending{false};
// The table the prompts are read from and the device they are drawn for.
const Bindings& shown() { return device.gamepad ? pad_assignments : assignments; }
bool initialized=false;
bool trace_owners=false;
std::set<std::string> owners_seen,decisions_seen;
uint32_t invalid_owners=0;
std::set<uint32_t> glyphs_seen;
uint32_t rejected_prompts=0;
// Tutorial pages pause gameplay but describe gameplay controls, so their prompts follow the gameplay bindings.
InputContext prompt_context(const ppc::Ctx& c) {
    const auto state=input_probe_snapshot(c);
    const auto context=game_input_context(state);
    return context==InputContext::menu && state.tutorial_loaded && state.tutorial_state!=0x00100000 ? InputContext::gameplay : context;
}
gx::Memory memory(const ppc::Ctx& c) {
    return {{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
}
}
void prompt_hooks_init(const Bindings& bindings,bool trace) {
    if(auto error=validate_bindings(bindings))throw std::invalid_argument(*error);
    registry={};assignments=bindings;pad_assignments=default_pad_bindings();device={};initialized=true;
    trace_owners=trace;owners_seen.clear();decisions_seen.clear();invalid_owners=0;glyphs_seen.clear();rejected_prompts=0;
}
void prompt_hooks_set_bindings(const Bindings& bindings) {
    if(auto error=validate_bindings(bindings))throw std::invalid_argument(*error);
    std::lock_guard lock(pending_mutex);pending=bindings;has_pending=true;
}
void prompt_hooks_set_gamepad(const Bindings& pad,const PromptDevice& next) {
    if(auto error=validate_pad_bindings(pad))throw std::invalid_argument(*error);
    std::lock_guard lock(pending_mutex);pending_pad=std::make_pair(pad,next);has_pending=true;
}
struct PromptSnapshot { PromptRegistry registry; Bindings assignments,pad_assignments; PromptDevice device; };
std::shared_ptr<const PromptSnapshot> prompt_checkpoint() {
    return std::make_shared<PromptSnapshot>(PromptSnapshot{registry,assignments,pad_assignments,device});
}
void prompt_restore(const PromptSnapshot& saved) { registry=saved.registry; assignments=saved.assignments; pad_assignments=saved.pad_assignments; device=saved.device; }
PromptScope::PromptScope(ppc::Ctx& c,uint32_t entry):c_(c),entry_(entry),exceptions_(std::uncaught_exceptions()) {
    if(!initialized || !c.mem)return;
    if(has_pending.exchange(false)) {
        std::lock_guard lock(pending_mutex);
        if(pending)assignments=std::move(*pending);pending.reset();
        if(pending_pad) {pad_assignments=std::move(pending_pad->first);device=pending_pad->second;}pending_pad.reset();
    }
    if(entry==0x8024F280) {object_=c.r[3];gate_=test_state_skip_tutorial(c);return;}   // original tutorial opener, r3 = tutorial object
    if(entry==0x8038AD94) {registry.unregister_layout(c.r[3]);return;}
    if(entry==0x80389B20) {registry.unregister_pane(c.r[3]);return;}
    if(entry==0x8038AEB0) {
        registry.unregister_layout(c.r[3]);
        if(c.lr!=0x80047CB0)return;
        try {
            const auto header=memory(c).read(c.r[4],16);
            if(header[0]!=std::byte{'R'} || header[1]!=std::byte{'L'} || header[2]!=std::byte{'Y'} || header[3]!=std::byte{'T'} || header[6]!=std::byte{0} || header[7]!=std::byte{8})return;
            const auto bytes=memory(c).read(c.r[29],64);
            const auto end=std::find(bytes.begin(),bytes.end(),std::byte{});
            if(end==bytes.end())return;
            name_.assign(reinterpret_cast<const char*>(bytes.data()),size_t(end-bytes.begin()));object_=c.r[3];
        } catch(const gx::Error&) {return;} // SDK profile mismatch leaves the original layout unchanged.
    } else if(entry==0x803B7CF0) {
        // CharWriter::Print(u16) from both TextWriterBase instantiations; PrintGlyph emits one quad.
        const uint16_t code=uint16_t(c.r[4]);
        if(code<0x391 || code>0x3A9 || (c.lr!=0x803BA238 && c.lr!=0x803BC138))return;
        const auto state=input_probe_snapshot(c);
        // Tutorial pages pause gameplay but describe gameplay controls.
        const bool menu=game_input_context(state)==InputContext::menu && !(state.tutorial_loaded && state.tutorial_state!=0x00100000);
        const auto actions=glyph_actions(code,menu);
        if(trace_owners && glyphs_seen.size()<64 && glyphs_seen.insert(code|uint32_t(menu)<<16).second)
            std::fprintf(stderr,"[prompt-glyph] U+%04X menu %u covered %u caller %08X\n",code,menu,!actions.empty(),c.lr);
        if(actions.empty())return;
        description_=actions.size()==1 ? primary_prompt(shown(),{actions[0]},device) : primary_prompt(shown(),{actions[0],actions[1]},device);
        compact_=true;checkpoint_=gx_prompt_checkpoint();
    } else if(entry==0x80394038 && c.lr==0x8038BD1C) {
        if(trace_owners && owners_seen.size()<256) {
            try {
            if(const auto layout=registry.layout_name(memory(c),c.r[28])) {
                const auto picture=memory(c).read(c.r[28],0xEC);
                const auto bytes=picture.subspan(0xB8,16);
                const auto end=std::find(bytes.begin(),bytes.end(),std::byte{});
                const std::string pane(reinterpret_cast<const char*>(bytes.data()),size_t(end-bytes.begin()));
                if(owners_seen.insert(*layout+":"+pane).second)std::fprintf(stderr,"[pane-owner] layout %s pane %s object %08X\n",layout->c_str(),pane.c_str(),c.r[28]);
            }
            } catch(const gx::Error& error) {if(++invalid_owners<=4)std::fprintf(stderr,"[pane-owner] unreadable picture: %s\n",error.what());}
        }
        if(trace_owners && decisions_seen.size()<256) {
            try {
                if(const auto layout=registry.layout_name(memory(c),c.r[28]); layout && layout->starts_with("ACT_")) {
                    const auto picture=memory(c).read(c.r[28],0xEC);
                    const auto bytes=picture.subspan(0xB8,16);
                    const std::string pane(reinterpret_cast<const char*>(bytes.data()),size_t(std::find(bytes.begin(),bytes.end(),std::byte{})-bytes.begin()));
                    const auto context=prompt_context(c);
                    const auto found=registry.actions(memory(c),c.r[28],context);
                    std::string actions;for(Action action:found)actions+=std::to_string(int(action))+",";
                    if(decisions_seen.insert(*layout+":"+pane+":"+std::to_string(unsigned(context))+":"+actions).second)
                        std::fprintf(stderr,"[prompt-decision] layout %s pane %s context %u actions %s\n",layout->c_str(),pane.c_str(),unsigned(context),actions.empty() ? "-" : actions.c_str());
                }
            } catch(const gx::Error&) {}
        }
        if(registry.actions(memory(c),c.r[28],InputContext::qte).empty())return;
        const auto found=registry.actions(memory(c),c.r[28],prompt_context(c));
        if(found.size()==1)description_=primary_prompt(shown(),{found[0]},device);   // the first assigned input only, never "X or Y"
        else if(found.size()==2)description_=chord_prompt(shown(),found[0],found[1],device);
        if(description_)checkpoint_=gx_prompt_checkpoint();
    }
}
PromptScope::~PromptScope() noexcept(false) {
    if(entry_==0x8024F280) {test_state_unskip_tutorial(c_,object_,gate_);return;}   // also while unwinding: the gate byte belongs to the game
    if(std::uncaught_exceptions()!=exceptions_)return;
    if(entry_==0x8038AEB0 && object_ && c_.r[3])registry.register_layout(memory(c_),object_,name_);
    if(!description_ || gx_prompt_attach(checkpoint_,*description_,compact_))return;
    if(trace_owners && rejected_prompts<16) {
        ++rejected_prompts;const auto now=gx_prompt_checkpoint();
        std::fprintf(stderr,"[prompt-reject] entry %08X list %08X->%08X recorded %zu->%zu batch %llu->%llu draws %zu->%zu producer %08X->%08X\n",entry_,checkpoint_.list,now.list,checkpoint_.recorded,now.recorded,
            (unsigned long long)checkpoint_.batch,(unsigned long long)now.batch,checkpoint_.draw,now.draw,checkpoint_.producer,now.producer);
    }
}
}
