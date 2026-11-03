#include "check.h"
#include "localization.h"
#include "settings_registry.h"
#include <set>
#include <string>
using namespace uc;
using namespace uc::settings;
int main() {
    const Text english=[](std::string_view key) { return std::string(launcher::text("en",key)); };
    Env env;
    {   // the table itself
        std::set<std::string> ids;
        for(const auto& item:items()) {
            CHECK(ids.insert(item.id).second);   // ids are unique
            CHECK(item.surfaces!=0);
            for(const auto& language:launcher::languages()) {   // every label and hint is translated into every language
                CHECK(!launcher::text(language.code,item.label_key).empty());
                if(item.hint_key) CHECK(!launcher::text(language.code,item.hint_key).empty());
            }
            switch(item.kind) {
            case Kind::toggle: CHECK(item.get_bool && item.set_bool); break;
            case Kind::slider: CHECK(item.get_int && item.set_int && item.min<item.max && item.step>0 && item.format); break;
            case Kind::choice: CHECK(item.labels && item.index && item.select); break;
            case Kind::color: CHECK(item.get_color && item.set_color); break;
            case Kind::custom: CHECK(!(item.surfaces&deck_menu) || std::string(item.id)=="pad_bindings" || std::string(item.id)=="mods_manager" || std::string(item.id)=="gyro_hold_button" || std::string(item.id)=="gyro_recenter" || std::string(item.id)=="quit_game"); break;   // the Deck menu draws rows only, but for its three pages of its own and the row that captures the gyro's hold button
            }
        }
        for(const auto page:{Page::game,Page::graphics,Page::effects,Page::controls,Page::gamepad,Page::reticle})
            for(const auto& language:launcher::languages()) CHECK(!launcher::text(language.code,page_label_key(page)).empty());
        CHECK(find("fps_limit") && !find("no_such_setting"));
    }
    {   // where things show
        for(const char* id:{"frame_generation","hdr_peak","hdr_paper_white","ray_tracing","dlss5","wiimote","pad_panel","key_bindings","renderer","window_mode","upscale_mode","sharpening"}) CHECK(!shown(id,deck_menu));   // nothing the Deck cannot do
        for(const char* id:{"hud_position","upscaling","ambient_occlusion","prompt_style","pointer_device","mouse_sensitivity"}) CHECK(!shown(id,deck_menu) && !shown(id,deck_launcher) && shown(id,desktop_menu));   // left out of the Deck on purpose
        for(const char* id:{"render_scale","fps_limit","anisotropy","bloom","hdr","hdr_ui","gyro_mode","gyro_sens_x","crosshair_scale","reload_zone","reload_zone_size","pointer_smoothing","rumble"}) CHECK(shown(id,deck_menu));
        CHECK(shown("rumble",deck_launcher) && shown("rumble",desktop_menu) && shown("rumble",desktop_launcher));   // everywhere a pad can be used
        for(const char* id:{"render_scale","fps_limit","dlss5","frame_generation","hdr","key_bindings","pad_panel"}) CHECK(shown(id,desktop_menu) || shown(id,desktop_launcher));
        CHECK(shown("skip_intro",desktop_launcher) && !shown("skip_intro",desktop_menu) && !shown("skip_intro",deck_menu));   // the launcher's own page
        CHECK(!shown("gyro_mode",desktop_menu));   // the desktop reaches the gyro through the Controllers window
        bool threw=false; try { shown("fps_limt",deck_menu); } catch(const std::logic_error&) { threw=true; }
        CHECK(threw);   // a typo in a menu is an error, not a hidden setting
        const auto pages=pages_for(deck_menu);
        CHECK(pages.size()==7 && pages[0]==Page::graphics && pages[1]==Page::effects && pages[2]==Page::gamepad && pages[3]==Page::buttons && pages[4]==Page::reticle && pages[5]==Page::mods && pages[6]==Page::quit);   // Graphics, Effects, Controls (the gamepad page), Buttons, Reticle, Mods, Quit
        CHECK(std::string(page_label_key(Page::gamepad))=="controls");
        for(const auto* item:items_for(deck_menu,Page::graphics)) CHECK(on_surface(*item,deck_menu));
        Options o; o.pointer=PointerMode::relative;
        CHECK(!shown("pointer_smoothing",deck_menu,o,env));   // conditional rows
        o.pointer=PointerMode::absolute; CHECK(shown("pointer_smoothing",deck_menu,o,env));
        o.reload_zone=ReloadZone::off; CHECK(!shown("reload_zone_size",deck_menu,o,env));
        o.reload_zone=ReloadZone::corner; CHECK(shown("reload_zone_size",deck_menu,o,env));
    }
    {   // every Deck item reads, changes and labels itself
        Options base;
        for(const auto surface:{deck_menu,desktop_menu}) for(const auto page:{Page::graphics,Page::effects,Page::controls,Page::gamepad,Page::reticle}) for(const auto* item:items_for(surface,page)) {
            Options o=base;
            switch(item->kind) {
            case Kind::toggle: {
                const bool before=item->get_bool(o);
                CHECK(flip(*item,o) && item->get_bool(o)!=before);
                CHECK(flip(*item,o) && item->get_bool(o)==before);
                CHECK(step(*item,o,env,1)==!before && step(*item,o,env,1)==false);   // right = on, again changes nothing
                break;
            }
            case Kind::slider: {
                CHECK(item->get_int(o)>=item->min && item->get_int(o)<=item->max);   // the defaults are inside the range
                set_slider(*item,o,item->min-1000); CHECK(item->get_int(o)==item->min);
                set_slider(*item,o,item->max+1000); CHECK(item->get_int(o)==item->max);
                set_slider(*item,o,item->min); CHECK(!step(*item,o,env,-1));   // the end of the range
                CHECK(step(*item,o,env,1) && item->get_int(o)==std::min(item->min+item->step,item->max));
                CHECK(!slider_text(*item,o).empty());
                break;
            }
            case Kind::choice: {
                const auto labels=item->labels(o,env,english);
                CHECK(labels.size()>=2);
                for(const auto& label:labels) CHECK(!label.empty());
                for(int i=0;i<int(labels.size());++i) { set_choice(*item,o,env,i); CHECK(item->index(o,env)==i); }
                CHECK(!set_choice(*item,o,env,int(labels.size())) && !set_choice(*item,o,env,-1));
                set_choice(*item,o,env,0); CHECK(!step(*item,o,env,-1) && step(*item,o,env,1) && item->index(o,env)==1);
                break;
            }
            case Kind::color: {
                Options p=base; p.reticle.color_p1=p.reticle.color_p2=0;
                CHECK(item->get_color(p)==0 && !step(*item,p,env,-1) && step(*item,p,env,1) && item->get_color(p)==color_presets()[1]);
                break;
            }
            case Kind::custom: break;
            }
        }
    }
    {   // particular items
        const auto& fps=*find("fps_limit"); Options o;
        const auto labels=fps.labels(o,env,english);
        CHECK(labels.front()=="30 FPS");
        set_choice(fps,o,env,int(labels.size())-1); CHECK(o.fps_limit.has_value());
        o.fps_limit=75; CHECK(fps.index(o,env)==-1);   // a value from the command line that the list does not hold
        CHECK(step(fps,o,env,1) && o.fps_limit==30);
        const auto& aniso=*find("anisotropy"); env.anisotropy_max=4; o.effects.anisotropy=4;
        CHECK(aniso.labels(o,env,english).size()==3);   // off, 2x, 4x
        o.effects.anisotropy=16; CHECK(aniso.labels(o,env,english).size()==4 && aniso.index(o,env)==3);   // a saved level the GPU cannot run stays visible
        env.anisotropy_max=16;
        const auto& up=*find("upscaling"); o=Options{}; env.vendor_upscalers=false;
        CHECK(up.labels(o,env,english).size()==2);   // Off, SMAA: no DLSS or FSR on the Deck
        set_choice(up,o,env,1); CHECK(o.upscaler==UpscalerKind::smaa && !o.frame_generation);
        env.vendor_upscalers=true; CHECK(up.labels(o,env,english).size()==4);
        env.features_known=true; env.effects=false; o=Options{};
        const auto& ppl=*find("per_pixel_lighting");
        CHECK(!ppl.state(o,env).enabled && ppl.state(o,env).why);   // greyed out on a GPU without the effects path
        o.effects.per_pixel_lighting=true; CHECK(ppl.state(o,env).enabled);   // but a value that is on can be switched off
        env=Env{}; o=Options{};
        const auto& gyro=*find("gyro_sens_x");
        set_slider(gyro,o,150); CHECK(o.gamepad.gyro.sensitivity_x==1.5f && gyro.get_int(o)==150);
        CHECK(!validate_gamepad(o.gamepad));   // every Deck value stays inside the gamepad settings' ranges
        set_slider(gyro,o,10000); CHECK(!validate_gamepad(o.gamepad));
        for(const auto* id:{"aim_speed","aim_dead_zone","camera_dead_zone","gyro_sens_y"}) { const auto& item=*find(id); set_slider(item,o,item.min); CHECK(!validate_gamepad(o.gamepad)); set_slider(item,o,item.max); CHECK(!validate_gamepad(o.gamepad)); }
        const auto& blur=*find("ambient_occlusion"); o=Options{}; o.effects.path_tracing=true;
        CHECK(!blur.state(o,env).enabled && std::string(blur.state(o,env).why)=="replaced_by_rt");   // path tracing lights the scene instead
    }
    return 0;
}
