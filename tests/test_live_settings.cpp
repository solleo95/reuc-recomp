#include "check.h"
#include "live_settings.h"
#include "options_config.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
namespace fs=std::filesystem;
int main() {
    const auto path=fs::temp_directory_path()/("uc-live-settings-"+std::to_string(GetCurrentProcessId())+".json");
    { std::ofstream(path,std::ios::binary) << R"({"game":"S:/Games/Test/game","graphics":{"render_scale":0,"bloom":true,"upscaler":"dlss","upscale_mode":"quality","frame_generation":true,"frame_generation_multiplier":3}})"; }
    std::vector<std::string> calls;
    uc::SettingsTargets targets;
    targets.render_scale=[&](double){ calls.push_back("render_scale"); };
    targets.upscaling=[&](const uc::Options&){ calls.push_back("upscaling"); };
    targets.fps_limit=[&](std::optional<uint32_t>){ calls.push_back("fps_limit"); };
    targets.bloom=[&](bool){ calls.push_back("bloom"); };
    targets.crosshair_scale=[&](uint32_t){ calls.push_back("crosshair_scale"); };
    targets.effects=[&](const uc::GraphicsEffects&){ calls.push_back("effects"); };
    targets.dlss5=[&](const uc::Dlss5Settings&){ calls.push_back("dlss5"); };
    targets.prompt_style=[&](uc::PromptStyle){ calls.push_back("prompt_style"); };
    targets.hdr=[&](const uc::HdrSettings&){ calls.push_back("hdr"); };
    targets.bindings=[&](const uc::Bindings&){ calls.push_back("bindings"); };
    uc::LiveSettings live(path,uc::load_config(path),targets);
    const auto saved=[&]{ return uc::options_from_config(uc::load_config(path)); };
    CHECK(live.options().upscaler==uc::UpscalerKind::dlss && live.options().render_scale==0);
    {   // only the changed field reaches its subsystem, and the file has it
        auto next=live.options(); next.bloom=false; live.apply(next);
        CHECK((calls==std::vector<std::string>{"bloom"}) && !saved().bloom && !live.error());
    }
    {   // the crosshair size alone reaches only its target
        calls.clear(); auto next=live.options(); next.crosshair_scale=40; live.apply(next);
        CHECK((calls==std::vector<std::string>{"crosshair_scale"}) && saved().crosshair_scale==40 && !live.error());
    }
    {   // HDR and its sliders reach only the HDR target
        calls.clear(); auto next=live.options(); next.hdr.enabled=true; next.hdr.peak_nits=800; live.apply(next);
        CHECK((calls==std::vector<std::string>{"hdr"}) && saved().hdr.enabled && saved().hdr.peak_nits==800 && !live.error());
    }
    {   // nothing changed: nothing called, the file is not rewritten
        calls.clear(); const auto before=fs::last_write_time(path);
        live.apply(live.options());
        CHECK(calls.empty() && fs::last_write_time(path)==before);
    }
    {   // the quality mode goes through the upscaling target once
        calls.clear(); auto next=live.options(); next.upscale_mode=uc::UpscaleMode::performance; live.apply(next);
        CHECK((calls==std::vector<std::string>{"upscaling"}) && saved().upscale_mode==uc::UpscaleMode::performance);
    }
    {   // an effects change reaches the effects target once and is saved
        calls.clear(); auto next=live.options(); next.effects.anisotropy=4; live.apply(next);
        CHECK((calls==std::vector<std::string>{"effects"}) && saved().effects.anisotropy==4);
        calls.clear(); next=live.options(); next.effects.per_pixel_lighting=true; live.apply(next);
        CHECK((calls==std::vector<std::string>{"effects"}) && saved().effects.per_pixel_lighting);
        calls.clear(); next=live.options(); next.effects.contact_shadows=true; live.apply(next);
        CHECK((calls==std::vector<std::string>{"effects"}) && saved().effects.contact_shadows);
        calls.clear(); next=live.options(); next.effects.motion_blur=uc::EffectLevel::medium; live.apply(next);
        CHECK((calls==std::vector<std::string>{"effects"}) && saved().effects.motion_blur==uc::EffectLevel::medium);
        calls.clear(); next=live.options(); next.effects.ambient_occlusion=uc::EffectLevel::high; live.apply(next);
        CHECK((calls==std::vector<std::string>{"effects"}) && saved().effects.ambient_occlusion==uc::EffectLevel::high);
        calls.clear(); next=live.options(); next.effects.path_tracing=true; live.apply(next);
        CHECK((calls==std::vector<std::string>{"effects"}) && saved().effects.path_tracing);
    }
    {   // DLSS 5 settings reach their own target once and are saved
        calls.clear(); auto next=live.options(); next.dlss5.hook=uc::Dlss5Hook::render; next.dlss5.passes=2; live.apply(next);
        CHECK((calls==std::vector<std::string>{"dlss5"}) && saved().dlss5.hook==uc::Dlss5Hook::render && saved().dlss5.passes==2 && !live.error());
        calls.clear(); live.apply(live.options()); CHECK(calls.empty());
    }
    {   // frame rate limit, unlimited included
        calls.clear(); auto next=live.options(); next.fps_limit=144u; live.apply(next);
        CHECK((calls==std::vector<std::string>{"fps_limit"}) && saved().fps_limit==144u);
        next.fps_limit=0u; live.apply(next); CHECK(saved().fps_limit==0u);
    }
    {   // render scale and prompt style
        calls.clear(); auto next=live.options(); next.render_scale=2; next.prompt_style=uc::PromptStyle::sculpted; live.apply(next);
        CHECK((calls==std::vector<std::string>{"render_scale","prompt_style"}) && saved().render_scale==2 && saved().prompt_style==uc::PromptStyle::sculpted);
    }
    {   // bindings
        calls.clear(); auto bindings=live.bindings(); bindings[size_t(uc::Action::fire)].clear(); live.apply(bindings);
        CHECK((calls==std::vector<std::string>{"bindings"}) && uc::load_config(path).input[size_t(uc::Action::fire)].empty());
    }
    {   // another writer replaced the file: the change still applies, the save is refused with an error
        { auto other=uc::load_config(path); other.properties["other_writer"]=true; uc::save_config(path,other); }
        calls.clear(); auto next=live.options(); next.bloom=true; live.apply(next);
        CHECK((calls==std::vector<std::string>{"bloom"}) && live.error() && live.error()->find("changed since loading")!=std::string::npos);
    }
    {   // frame generation follows its upscaler
        uc::Options o; o.upscaler=uc::UpscalerKind::fsr;
        auto dlss=uc::with_frame_generation(o,uc::FrameGenChoice::dlss);
        CHECK(dlss.upscaler==uc::UpscalerKind::dlss && dlss.frame_generation && dlss.frame_generation_multiplier==2);
        o.upscaler=uc::UpscalerKind::dlss; o.frame_generation_multiplier=3;
        CHECK(uc::with_frame_generation(o,uc::FrameGenChoice::dlss).frame_generation_multiplier==3);   // a launcher DLSS 3x stays
        auto fsr=uc::with_frame_generation(o,uc::FrameGenChoice::fsr);
        CHECK(fsr.upscaler==uc::UpscalerKind::fsr && fsr.frame_generation && fsr.frame_generation_multiplier==2);
        CHECK(!uc::with_frame_generation(fsr,uc::FrameGenChoice::off).frame_generation);
        CHECK(!uc::with_upscaler(fsr,uc::UpscalerKind::off).frame_generation);
        CHECK(!uc::with_upscaler(fsr,uc::UpscalerKind::smaa).frame_generation);   // SMAA has no generator
        CHECK(uc::frame_generation_choice(fsr)==uc::FrameGenChoice::fsr && uc::frame_generation_choice(uc::Options{})==uc::FrameGenChoice::off);
    }
    fs::remove(path);
}
