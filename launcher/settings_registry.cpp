#include "settings_registry.h"
#include "frame_limit_cap.h"
#include "live_settings.h"
#include "render_scale.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
namespace uc::settings {
namespace {
constexpr uint32_t kLimits[]={30,60,90,120,144,165,240,360,0};   // the launcher's list; 0 = unlimited
constexpr double kScales[]={0.,1.,1.5,2.,3.};
constexpr uint32_t kAnisotropy[]={1,2,4,8,16};
constexpr uint32_t kColors[]={0,0xFF3030,0xFF8A1E,0xFFD21E,0x3CD25A,0x1ED2D2,0x3264FF,0xA050FF,0xFFFFFF};   // the game's own, red, orange, yellow, green, cyan, blue, violet, white
constexpr UpscalerKind kUpscalers[]={UpscalerKind::off,UpscalerKind::smaa,UpscalerKind::dlss,UpscalerKind::fsr};
constexpr uint8_t DM=desktop_menu, KM=deck_menu, DL=desktop_launcher, KL=deck_launcher;

Item toggle(const char* id,const char* label,Page page,uint8_t surfaces,bool(*get)(const Options&),void(*set)(Options&,bool),State(*state)(const Options&,const Env&)=nullptr,const char* hint=nullptr) {
    Item item{id,label,hint,page,Kind::toggle,surfaces,state}; item.get_bool=get; item.set_bool=set; return item;
}
Item slider(const char* id,const char* label,Page page,uint8_t surfaces,int(*get)(const Options&),void(*set)(Options&,int),int low,int high,int step,const char* format,State(*state)(const Options&,const Env&)=nullptr,const char* hint=nullptr) {
    Item item{id,label,hint,page,Kind::slider,surfaces,state}; item.get_int=get; item.set_int=set; item.min=low; item.max=high; item.step=step; item.format=format; return item;
}
Item choice(const char* id,const char* label,Page page,uint8_t surfaces,std::vector<std::string>(*labels)(const Options&,const Env&,const Text&),int(*index)(const Options&,const Env&),void(*select)(Options&,const Env&,int),State(*state)(const Options&,const Env&)=nullptr,const char* hint=nullptr) {
    Item item{id,label,hint,page,Kind::choice,surfaces,state}; item.labels=labels; item.index=index; item.select=select; return item;
}
Item color(const char* id,const char* label,Page page,uint8_t surfaces,uint32_t(*get)(const Options&),void(*set)(Options&,uint32_t)) {
    Item item{id,label,nullptr,page,Kind::color,surfaces}; item.get_color=get; item.set_color=set; return item;
}
Item custom(const char* id,const char* label,Page page,uint8_t surfaces) { return Item{id,label,nullptr,page,Kind::custom,surfaces}; }

int percent(float value) { return int(std::lround(double(value)*100)); }
// The screen-space effects are greyed out on a GPU without their path; a value that is on stays usable so it can be switched off.
bool effects_blocked(const Env& env) { return env.features_known && !env.effects; }
bool raster_replaced(const Options& o,const Env& env) { return env.backend.ray_tracing && o.effects.path_tracing && !effects_blocked(env); }   // path tracing lights the scene instead
State effect_state(bool on,const Env& env) { return effects_blocked(env) && !on ? State{true,false,"effects_unavailable"} : State{}; }
std::vector<std::string> level_labels(const Options&,const Env&,const Text& tr) { return {tr("off"),tr("level_low"),tr("level_medium"),tr("level_high"),tr("level_ultra")}; }
std::vector<std::string> rt_labels(const Options&,const Env&,const Text& tr) { return {tr("level_low"),tr("level_medium"),tr("level_high"),tr("level_ultra")}; }

std::vector<std::string> scale_labels(const Options&,const Env& env,const Text& tr) {
    std::vector<std::string> labels;
    for(const double scale:kScales) {
        const double effective=scale ? scale : monitor_render_scale(env.monitor_width,env.monitor_height);
        const auto extent=manual_render_extent(effective,double(env.monitor_width)/env.monitor_height);
        char value[100]; std::snprintf(value,sizeof value,"%.2g× · %u × %u",effective,extent.width,extent.height);
        labels.push_back(scale ? (scale==1 ? "1× (Native) · "+std::to_string(extent.width)+" × "+std::to_string(extent.height) : std::string(value))
                               : tr("monitor")+" · "+std::to_string(extent.width)+" × "+std::to_string(extent.height));
    }
    return labels;
}
std::vector<uint32_t> allowed_limits() {
    std::vector<uint32_t> limits;
    for(const uint32_t value:kLimits) if(!(kMaxFrameLimit && (value==0 || value>kMaxFrameLimit))) limits.push_back(value);   // Linux: nothing above the cap, and no "unlimited"
    return limits;
}
uint32_t current_limit(const Options& o) { return o.fps_limit.value_or(o.unlock_60fps ? 60 : 30); }
std::vector<std::string> fps_labels(const Options&,const Env&,const Text& tr) {
    std::vector<std::string> labels;
    for(const uint32_t value:allowed_limits()) labels.push_back(value ? std::to_string(value)+" FPS" : tr("unlimited"));
    return labels;
}
int fps_index(const Options& o,const Env&) {
    const auto limits=allowed_limits(); const auto at=std::find(limits.begin(),limits.end(),current_limit(o));
    return at==limits.end() ? -1 : int(at-limits.begin());
}
size_t upscaler_count(const Env& env) { return env.vendor_upscalers ? (env.backend.fsr ? 4 : 3) : 2; }
std::vector<std::string> upscaling_labels(const Options&,const Env& env,const Text& tr) {
    std::vector<std::string> labels{tr("off"),"SMAA"};
    if(env.vendor_upscalers) { labels.push_back("DLSS"); if(env.backend.fsr) labels.push_back("FSR"); }
    return labels;
}
int upscaling_index(const Options& o,const Env& env) {
    for(size_t i=0;i<upscaler_count(env);++i) if(kUpscalers[i]==o.upscaler) return int(i);
    return -1;
}
std::vector<uint32_t> anisotropy_levels(const Options& o,const Env& env) {   // the levels this GPU runs, and a saved level it cannot (so it stays visible and can be left)
    std::vector<uint32_t> levels;
    for(const uint32_t level:kAnisotropy) if(level<=env.anisotropy_max || level==o.effects.anisotropy) levels.push_back(level);
    return levels;
}
std::vector<std::string> anisotropy_labels(const Options& o,const Env& env,const Text& tr) {
    std::vector<std::string> labels;
    for(const uint32_t level:anisotropy_levels(o,env)) labels.push_back(level==1 ? tr("off") : std::to_string(level)+"×");
    return labels;
}

State smoothing_state(const Options& o,const Env&) { return o.pointer==PointerMode::absolute ? State{} : State{false,false,nullptr}; }   // only an absolute pointer is smoothed
State sensitivity_state(const Options& o,const Env&) { return State{true,o.pointer!=PointerMode::absolute,nullptr}; }   // the cursor's place is the aim: the speed means nothing
State zone_size_state(const Options& o,const Env&) { return o.reload_zone==ReloadZone::off ? State{false,false,nullptr} : State{}; }
State sharpness_state(const Options& o,const Env&) { return State{true,o.sharpening && o.upscaler!=UpscalerKind::off,nullptr}; }
State bloom_strength_state(const Options& o,const Env&) { return State{true,o.bloom,nullptr}; }
State hdr_slider_state(const Options& o,const Env&) { return State{true,o.hdr.enabled,nullptr}; }
State tail_state(const Options& o,const Env&) { return State{true,!o.reticle.hide_tail,nullptr}; }
State dot_state(const Options& o,const Env&) { return State{true,!o.reticle.hide_dot,nullptr}; }
State upscaling_state(const Options&,const Env& env) { return effects_blocked(env) ? State{true,false,"effects_unavailable"} : State{}; }
State ppl_state(const Options& o,const Env& e) { return effect_state(o.effects.per_pixel_lighting,e); }
State ao_state(const Options& o,const Env& e) { return raster_replaced(o,e) ? State{true,false,"replaced_by_rt"} : effect_state(o.effects.ambient_occlusion!=EffectLevel::off,e); }
State blur_state(const Options& o,const Env& e) { return effect_state(o.effects.motion_blur!=EffectLevel::off,e); }
State contact_state(const Options& o,const Env& e) { return raster_replaced(o,e) ? State{true,false,"replaced_by_rt"} : effect_state(o.effects.contact_shadows,e); }
State ray_state(const Options&,const Env& e) { return e.backend.ray_tracing ? State{} : State{false,false,nullptr}; }
State rt_quality_state(const Options& o,const Env& e) { return e.backend.ray_tracing ? State{true,o.effects.path_tracing,nullptr} : State{false,false,nullptr}; }

const std::vector<Item>& table() {
    static const std::vector<Item> rows=[] {
        std::vector<Item> t;
        constexpr Page G=Page::game, V=Page::graphics, E=Page::effects, C=Page::controls, P=Page::gamepad, R=Page::reticle;
        // Game: the launcher's page
        t.push_back(toggle("skip_intro","skip",G,DL|KL,[](const Options& o) { return o.skip_intro; },[](Options& o,bool v) { o.skip_intro=v; }));
        t.push_back(toggle("skip_tutorials","skip_tutorials",G,DL|KL,[](const Options& o) { return o.skip_tutorials; },[](Options& o,bool v) { o.skip_tutorials=v; }));
        t.push_back(toggle("logs","enable_logs",G,DL|KL,[](const Options& o) { return o.logs; },[](Options& o,bool v) { o.logs=v; }));
        // Graphics
        t.push_back(choice("renderer","renderer",V,DL,
            [](const Options&,const Env&,const Text&) { return std::vector<std::string>{"Vulkan","DirectX 11","DirectX 12"}; },
            [](const Options& o,const Env&) { return int(o.renderer_backend); },[](Options& o,const Env&,int i) { o.renderer_backend=RendererBackend(i); }));
        t.push_back(choice("window_mode","window",V,DL,
            [](const Options&,const Env&,const Text& tr) { return std::vector<std::string>{tr("windowed"),tr("borderless"),tr("exclusive")}; },
            [](const Options& o,const Env&) { return int(o.window_mode); },[](Options& o,const Env&,int i) { o.window_mode=WindowMode(i); }));
        t.push_back(choice("render_scale","scale",V,kEverywhere,scale_labels,
            [](const Options& o,const Env&) { for(size_t i=0;i<std::size(kScales);++i) if(kScales[i]==o.render_scale) return int(i); return -1; },
            [](Options& o,const Env&,int i) { o.render_scale=kScales[size_t(i)]; }));
        t.push_back(choice("fps_limit","fps_limit",V,kEverywhere,fps_labels,fps_index,[](Options& o,const Env&,int i) { o.fps_limit=allowed_limits()[size_t(i)]; }));
        t.push_back(choice("upscaling","upscaling",V,DM|DL,upscaling_labels,upscaling_index,
            [](Options& o,const Env&,int i) { o=with_upscaler(o,kUpscalers[size_t(i)]); },upscaling_state));
        t.push_back(custom("upscale_mode","quality",V,DM|DL));
        t.push_back(toggle("sharpening","sharpening",V,DM|DL,[](const Options& o) { return o.sharpening; },[](Options& o,bool v) { o.sharpening=v; }));
        t.push_back(slider("sharpness","sharpening",V,DM|DL,[](const Options& o) { return int(std::lround(o.sharpness*100)); },[](Options& o,int v) { o.sharpness=v/100.; },1,100,1,"%d%%",sharpness_state));
        t.push_back(custom("frame_generation","framegen",V,DM|DL));
        t.push_back(choice("hud_position","hud_position",V,DM|DL,
            [](const Options&,const Env&,const Text& tr) { return std::vector<std::string>{tr("hud_auto"),tr("hud_center"),tr("hud_edges")}; },
            [](const Options& o,const Env&) { return int(o.hud_position); },[](Options& o,const Env&,int i) { o.hud_position=HudPosition(i); },nullptr,"hud_hint"));
        t.push_back(toggle("hdr","hdr",V,DM|DL,[](const Options& o) { return o.hdr.enabled; },[](Options& o,bool v) { o.hdr.enabled=v; },nullptr,"hdr_hint"));
        t.push_back(slider("hdr_peak","hdr_peak",V,DM|DL,[](const Options& o) { return int(o.hdr.peak_nits); },[](Options& o,int v) { o.hdr.peak_nits=uint32_t(v); },int(kHdrPeakMin),int(kHdrPeakMax),10,"%d nits",hdr_slider_state));
        t.push_back(slider("hdr_paper_white","hdr_paper_white",V,DM|DL,[](const Options& o) { return int(o.hdr.paper_white_nits); },[](Options& o,int v) { o.hdr.paper_white_nits=uint32_t(v); },int(kHdrWhiteMin),int(kHdrWhiteMax),5,"%d nits",hdr_slider_state));
        t.push_back(slider("hdr_ui","hdr_ui",V,DM|DL,[](const Options& o) { return int(o.hdr.ui_nits); },[](Options& o,int v) { o.hdr.ui_nits=uint32_t(v); },int(kHdrWhiteMin),int(kHdrWhiteMax),5,"%d nits",hdr_slider_state));
        // Effects
        t.push_back(choice("anisotropy","anisotropy",E,kEverywhere,anisotropy_labels,
            [](const Options& o,const Env& e) { const auto levels=anisotropy_levels(o,e); const auto at=std::find(levels.begin(),levels.end(),o.effects.anisotropy); return at==levels.end() ? -1 : int(at-levels.begin()); },
            [](Options& o,const Env& e,int i) { o.effects.anisotropy=anisotropy_levels(o,e)[size_t(i)]; }));
        t.push_back(toggle("per_pixel_lighting","per_pixel_lighting",E,kEverywhere,[](const Options& o) { return o.effects.per_pixel_lighting; },[](Options& o,bool v) { o.effects.per_pixel_lighting=v; },ppl_state));
        t.push_back(choice("ambient_occlusion","ambient_occlusion",E,DM|DL,level_labels,   // the Deck keeps it off (platform_ambient_occlusion)
            [](const Options& o,const Env&) { return int(o.effects.ambient_occlusion); },[](Options& o,const Env&,int i) { o.effects.ambient_occlusion=EffectLevel(i); },ao_state));
        t.push_back(choice("motion_blur","motion_blur",E,kEverywhere,level_labels,
            [](const Options& o,const Env&) { return int(o.effects.motion_blur); },[](Options& o,const Env&,int i) { o.effects.motion_blur=EffectLevel(i); },blur_state));
        t.push_back(toggle("contact_shadows","contact_shadows",E,kEverywhere,[](const Options& o) { return o.effects.contact_shadows; },[](Options& o,bool v) { o.effects.contact_shadows=v; },contact_state));
        t.push_back(toggle("ray_tracing","ray_tracing",E,DM|DL,[](const Options& o) { return o.effects.path_tracing; },[](Options& o,bool v) { o.effects.path_tracing=v; },ray_state));
        t.push_back(choice("rt_quality","rt_quality",E,DM|DL,rt_labels,
            [](const Options& o,const Env&) { const int i=int(o.effects.rt_quality)-1; return i<0 ? 2 : i; },[](Options& o,const Env&,int i) { o.effects.rt_quality=EffectLevel(i+1); },rt_quality_state));
        t.push_back(toggle("bloom","bloom",E,kEverywhere,[](const Options& o) { return o.bloom; },[](Options& o,bool v) { o.bloom=v; }));
        t.push_back(slider("bloom_strength","bloom_strength",E,kEverywhere,[](const Options& o) { return int(o.bloom_strength); },[](Options& o,int v) { o.bloom_strength=uint32_t(v); },0,int(kBloomStrengthMax),5,"%d%%",bloom_strength_state));
        t.push_back(custom("dlss5","dlss5",E,DM|DL));
        // Controls
        t.push_back(choice("pointer_device","pointer_device",C,DM|DL,
            [](const Options&,const Env&,const Text& tr) { return std::vector<std::string>{tr("pointer_mouse"),tr("pointer_lightgun")}; },
            [](const Options& o,const Env&) { return int(o.pointer); },[](Options& o,const Env&,int i) { o.pointer=PointerMode(i); }));
        t.push_back(slider("mouse_sensitivity","mouse_sensitivity",C,DM|DL,[](const Options& o) { return int(o.mouse_sensitivity); },[](Options& o,int v) { o.mouse_sensitivity=uint32_t(v); },int(kMouseSensitivityMin),int(kMouseSensitivityMax),5,"%d%%",sensitivity_state));
        t.push_back(choice("prompt_style","style",C,DM|DL,
            [](const Options&,const Env&,const Text& tr) { return std::vector<std::string>{tr("cyan"),tr("gray")}; },   // cyan (Style 1) first, as the menu lists them
            [](const Options& o,const Env&) { return o.prompt_style==PromptStyle::sculpted ? 0 : 1; },[](Options& o,const Env&,int i) { o.prompt_style=i==0 ? PromptStyle::sculpted : PromptStyle::monochrome; }));
        t.push_back(custom("key_bindings","bindings",C,DM|DL));
        // Gamepad: on the desktop the shared Controllers window holds all of these; the Deck menu draws them as rows
        t.push_back(custom("pad_panel","gamepad",P,DM|DL));
        t.push_back(custom("wiimote","wiimote",P,DM|DL));
        t.push_back(choice("gyro_mode","gyro_mode",P,KM|KL,
            [](const Options&,const Env&,const Text& tr) { return std::vector<std::string>{tr("off"),tr("gyro_always"),tr("gyro_hold")}; },
            [](const Options& o,const Env&) { return int(o.gamepad.gyro.mode); },[](Options& o,const Env&,int i) { o.gamepad.gyro.mode=GyroMode(i); }));
        t.push_back(slider("gyro_sens_x","gyro_sens_x",P,KM|KL,[](const Options& o) { return percent(o.gamepad.gyro.sensitivity_x); },[](Options& o,int v) { o.gamepad.gyro.sensitivity_x=float(v)/100; },10,500,5,"%d%%"));
        t.push_back(slider("gyro_sens_y","gyro_sens_y",P,KM|KL,[](const Options& o) { return percent(o.gamepad.gyro.sensitivity_y); },[](Options& o,int v) { o.gamepad.gyro.sensitivity_y=float(v)/100; },10,500,5,"%d%%"));
        t.push_back(toggle("gyro_invert_x","gyro_invert_x",P,KM|KL,[](const Options& o) { return o.gamepad.gyro.invert_x; },[](Options& o,bool v) { o.gamepad.gyro.invert_x=v; }));
        t.push_back(toggle("gyro_invert_y","gyro_invert_y",P,KM|KL,[](const Options& o) { return o.gamepad.gyro.invert_y; },[](Options& o,bool v) { o.gamepad.gyro.invert_y=v; }));
        t.push_back(slider("aim_speed","aim_speed",P,KM|KL,[](const Options& o) { return percent(o.gamepad.aim.speed); },[](Options& o,int v) { o.gamepad.aim.speed=float(v)/100; },20,400,10,"%d%%"));
        t.push_back(slider("aim_dead_zone","deck_aim_dead_zone",P,KM|KL,[](const Options& o) { return percent(o.gamepad.aim.dead_zone); },[](Options& o,int v) { o.gamepad.aim.dead_zone=float(v)/100; },0,50,1,"%d%%"));
        t.push_back(slider("camera_dead_zone","deck_camera_dead_zone",P,KM|KL,[](const Options& o) { return percent(o.gamepad.camera_dead_zone); },[](Options& o,int v) { o.gamepad.camera_dead_zone=float(v)/100; },0,50,1,"%d%%"));
        // Touch aim: on the Deck it sits on the Controls page with the gyro and sticks; the desktop menus keep it in their own widgets
        t.push_back(slider("pointer_smoothing","pointer_smoothing",P,kEverywhere,[](const Options& o) { return int(o.pointer_smoothing); },[](Options& o,int v) { o.pointer_smoothing=uint32_t(v); },0,int(kPointerSmoothingMax),5,"%d%%",smoothing_state));
        t.push_back(choice("reload_zone","reload_zone",P,kEverywhere,
            [](const Options&,const Env&,const Text& tr) { return std::vector<std::string>{tr("reload_zone_off"),tr("reload_zone_corner"),tr("reload_zone_edge")}; },
            [](const Options& o,const Env&) { return int(o.reload_zone); },[](Options& o,const Env&,int i) { o.reload_zone=ReloadZone(i); }));
        t.push_back(slider("reload_zone_size","reload_zone_size",P,kEverywhere,[](const Options& o) { return int(o.reload_zone_size); },[](Options& o,int v) { o.reload_zone_size=uint32_t(v); },int(kReloadZoneSizeMin),int(kReloadZoneSizeMax),1,"%d%%",zone_size_state));
        // Deck pages the menu draws itself: the gamepad's buttons and the mod manager
        t.push_back(custom("pad_bindings","bindings",Page::buttons,KM));
        // Reticle: the in-game menus (the launcher has no reticle page)
        t.push_back(custom("reticle_preview","reticle_preview",R,DM));
        t.push_back(slider("crosshair_scale","crosshair_scale",R,DM|KM,[](const Options& o) { return int(o.crosshair_scale); },[](Options& o,int v) { o.crosshair_scale=uint32_t(v); },0,100,5,"%d%%"));
        t.push_back(toggle("hide_tail","hide_tail",R,DM|KM,[](const Options& o) { return o.reticle.hide_tail; },[](Options& o,bool v) { o.reticle.hide_tail=v; }));
        t.push_back(toggle("hide_dot","hide_dot",R,DM|KM,[](const Options& o) { return o.reticle.hide_dot; },[](Options& o,bool v) { o.reticle.hide_dot=v; }));
        t.push_back(toggle("hide_marks","hide_marks",R,DM|KM,[](const Options& o) { return o.reticle.hide_marks; },[](Options& o,bool v) { o.reticle.hide_marks=v; }));
        t.push_back(toggle("hide_arc","hide_arc",R,DM|KM,[](const Options& o) { return o.reticle.hide_arc; },[](Options& o,bool v) { o.reticle.hide_arc=v; }));
        t.push_back(slider("tail_length","tail_length",R,DM|KM,[](const Options& o) { return int(o.reticle.tail_length); },[](Options& o,int v) { o.reticle.tail_length=uint32_t(v); },int(kReticleTailMin),int(kReticleTailMax),5,"%d%%",tail_state));
        t.push_back(slider("dot_size","dot_size",R,DM|KM,[](const Options& o) { return int(o.reticle.dot_size); },[](Options& o,int v) { o.reticle.dot_size=uint32_t(v); },int(kReticleDotMin),int(kReticleDotMax),5,"%d%%",dot_state));
        t.push_back(slider("partner_opacity","partner_opacity",R,DM|KM,[](const Options& o) { return int(o.reticle.partner_opacity); },[](Options& o,int v) { o.reticle.partner_opacity=uint32_t(v); },0,100,5,"%d%%"));
        t.push_back(color("reticle_color_p1","reticle_color_p1",R,DM|KM,[](const Options& o) { return o.reticle.color_p1; },[](Options& o,uint32_t v) { o.reticle.color_p1=v; }));
        t.push_back(color("reticle_color_p2","reticle_color_p2",R,DM|KM,[](const Options& o) { return o.reticle.color_p2; },[](Options& o,uint32_t v) { o.reticle.color_p2=v; }));
        t.push_back(custom("mods_manager","deck_page_mods",Page::mods,KM));   // the mod platform's own manager (it exists only while mods are on: the Deck menu hides the page otherwise)
        return t;
    }();
    return rows;
}
const Item& need(std::string_view id) {
    if(const auto* item=find(id)) return *item;
    throw std::logic_error("unknown setting: "+std::string(id));
}
}
std::span<const Item> items() { return table(); }
const Item* find(std::string_view id) {
    for(const auto& item:table()) if(id==item.id) return &item;
    return nullptr;
}
bool on_surface(const Item& item,Surface surface) { return (item.surfaces&surface)!=0; }
bool shown(std::string_view id,Surface surface) { return on_surface(need(id),surface); }
bool shown(std::string_view id,Surface surface,const Options& options,const Env& env) {
    const auto& item=need(id);
    return on_surface(item,surface) && (!item.state || item.state(options,env).visible);
}
std::vector<const Item*> items_for(Surface surface,Page page) {
    std::vector<const Item*> result;
    for(const auto& item:table()) if(item.page==page && on_surface(item,surface)) result.push_back(&item);
    return result;
}
std::vector<Page> pages_for(Surface surface) {
    std::vector<Page> pages;
    for(const auto page:{Page::game,Page::graphics,Page::effects,Page::controls,Page::gamepad,Page::buttons,Page::reticle,Page::mods}) if(!items_for(surface,page).empty()) pages.push_back(page);
    return pages;
}
const char* page_label_key(Page page) {
    switch(page) {
    case Page::game: return "game";
    case Page::graphics: return "graphics_title";
    case Page::effects: return "effects";
    case Page::controls: return "controls";
    case Page::gamepad: return "controls";   // the Deck's one Controls page (the gyro, the sticks, touch aim); the desktop menus do not list pages
    case Page::buttons: return "bindings";
    case Page::reticle: return "reticle";
    case Page::mods: return "deck_page_mods";
    }
    return "";
}
std::span<const uint32_t> color_presets() { return kColors; }
bool flip(const Item& item,Options& options) {
    if(item.kind!=Kind::toggle) return false;
    item.set_bool(options,!item.get_bool(options));
    return true;
}
bool set_slider(const Item& item,Options& options,int value) {
    if(item.kind!=Kind::slider) return false;
    const int before=item.get_int(options);
    value=std::clamp(value,item.min,item.max);
    value=std::clamp(item.min+int(std::lround(double(value-item.min)/item.step))*item.step,item.min,item.max);   // snapped to the step; the end of the range is always reachable
    if(value==before) return false;
    item.set_int(options,value);
    return item.get_int(options)!=before;
}
bool set_choice(const Item& item,Options& options,const Env& env,int index) {
    if(item.kind!=Kind::choice) return false;
    const int count=int(item.labels(options,env,[](std::string_view) { return std::string(); }).size());
    if(index<0 || index>=count || index==item.index(options,env)) return false;
    item.select(options,env,index);
    return true;
}
bool step(const Item& item,Options& options,const Env& env,int delta) {
    delta=delta<0 ? -1 : 1;
    switch(item.kind) {
    case Kind::toggle: {
        const bool want=delta>0;
        if(item.get_bool(options)==want) return false;
        item.set_bool(options,want); return true;
    }
    case Kind::slider: {
        const int now=item.get_int(options);
        return set_slider(item,options,now+delta*item.step);
    }
    case Kind::choice: {
        const int count=int(item.labels(options,env,[](std::string_view) { return std::string(); }).size()), at=item.index(options,env);
        return set_choice(item,options,env,at<0 ? (delta>0 ? 0 : count-1) : at+delta);
    }
    case Kind::color: {
        const auto presets=color_presets(); const uint32_t now=item.get_color(options);
        int at=-1; for(size_t i=0;i<presets.size();++i) if(presets[i]==now) at=int(i);
        const int next=at<0 ? (delta>0 ? 0 : int(presets.size())-1) : at+delta;
        if(next<0 || next>=int(presets.size()) || presets[size_t(next)]==now) return false;
        item.set_color(options,presets[size_t(next)]); return true;
    }
    case Kind::custom: return false;
    }
    return false;
}
std::string slider_text(const Item& item,const Options& options) {
    char text[64]; std::snprintf(text,sizeof text,item.format,item.get_int(options));
    return text;
}
}
