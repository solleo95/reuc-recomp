#pragma once
#include "ppc_rt.h"
#include "input_prompts.h"
#include "hle/gx.h"
namespace uc {
void prompt_hooks_init(const Bindings&,bool trace=false);
void prompt_hooks_set_bindings(const Bindings&);   // any thread (in-game menu); prompts built from the next hook on use them
// Any thread: the gamepad table and which device the prompts show (keyboard and mouse until this is called); effective from the next hook on.
void prompt_hooks_set_gamepad(const Bindings& pad,const PromptDevice&);
struct PromptSnapshot;
std::shared_ptr<const PromptSnapshot> prompt_checkpoint();
void prompt_restore(const PromptSnapshot&);
class PromptScope {
public:
    PromptScope(ppc::Ctx&,uint32_t entry);
    ~PromptScope() noexcept(false);
    PromptScope(const PromptScope&)=delete;
    PromptScope& operator=(const PromptScope&)=delete;
private:
    ppc::Ctx& c_;
    uint32_t entry_,object_=0;
    int gate_=-1;
    int exceptions_;
    std::string name_;
    std::optional<PromptDescription> description_;
    bool compact_=false;
    GxPromptCheckpoint checkpoint_{};
};
struct NoPromptScope {};
template<uint32_t entry> auto prompt_scope(ppc::Ctx& c) {
    if constexpr(entry==0x8038AEB0 || entry==0x8038AD94 || entry==0x80389B20 || entry==0x80394038 || entry==0x803B7CF0 || entry==0x8024F280)
        return PromptScope(c,entry);
    else return NoPromptScope{};
}
}
