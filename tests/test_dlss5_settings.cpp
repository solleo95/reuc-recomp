#include "check.h"
#include "dlss5_settings.h"
#include "dlss5_math.h"
#include <cmath>
#include <set>
#include <string>
using namespace uc;
namespace {
bool near(double a, double b, double eps=1e-6) { return std::fabs(a-b)<=eps; }
bool set(Dlss5Settings& s, std::string_view key, std::string_view text) { std::string error; return set_dlss5_field(s,key,text,error); }
}
int main() {
    // The table: unique keys, every default inside its own range, getters and setters agree.
    std::set<std::string_view> keys;
    const Dlss5Settings defaults;
    for(const auto& f:dlss5_fields()) {
        CHECK(keys.insert(f.key).second);
        CHECK(dlss5_value_error(f,f.get(defaults)).empty());
        Dlss5Settings copy; f.set(copy,f.get(defaults)); CHECK(copy==defaults);
        if(f.kind==Dlss5Kind::choice) { CHECK(!f.choices.empty() && f.max==double(f.choices.size()-1)); CHECK(f.min==0); }
    }
    CHECK(keys.size()>=20 && !defaults.enabled && defaults.stage==Dlss5Stage::upscaled && defaults.passes==1 && defaults.processing_scale==1.0);
    // Parsing is strict: whole text, type and range.
    Dlss5Settings s;
    CHECK(set(s,"enabled","true") && s.enabled);
    CHECK(set(s,"enabled","false") && !s.enabled);
    CHECK(!set(s,"enabled","1") && !set(s,"enabled","") && !set(s,"enabled","True"));
    CHECK(set(s,"passes","3") && s.passes==3);
    CHECK(!set(s,"passes","0") && !set(s,"passes","5") && !set(s,"passes","2x") && !set(s,"passes","-1") && !set(s,"passes","2.0") && s.passes==3);
    CHECK(set(s,"processing_scale","0.75") && near(s.processing_scale,.75));
    CHECK(!set(s,"processing_scale","0.4") && !set(s,"processing_scale","1.01") && !set(s,"processing_scale","nan") && !set(s,"processing_scale","inf") && !set(s,"processing_scale","0.5x") && near(s.processing_scale,.75));
    CHECK(set(s,"preset","h") && s.preset==Dlss5Preset::h && set(s,"preset","b") && s.preset==Dlss5Preset::b);
    CHECK(!set(s,"preset","c") && !set(s,"preset","H") && !set(s,"preset","") && s.preset==Dlss5Preset::b);
    CHECK(set(s,"stage","present") && s.stage==Dlss5Stage::present && set(s,"stage","render") && s.stage==Dlss5Stage::render && !set(s,"stage","hud"));
    CHECK(set(s,"signal_encoding","gamma24") && s.signal_encoding==Dlss5Encoding::gamma24);
    CHECK(set(s,"downsample_filter","bicubic") && s.downsample_filter==Dlss5Filter::bicubic && set(s,"upsample_filter","nearest") && s.upsample_filter==Dlss5Filter::nearest);
    CHECK(set(s,"scaling_domain","linear") && s.scaling_domain==Dlss5Domain::linear);
    CHECK(set(s,"ui_correction","on") && s.ui_correction==Dlss5UiCorrection::on);
    CHECK(set(s,"exposure_mode","manual") && s.exposure_mode==Dlss5ExposureMode::manual && set(s,"exposure","2") && near(s.exposure,2));
    CHECK(!set(s,"exposure","0.1") && !set(s,"exposure","8"));
    CHECK(set(s,"diffuse_white_nits","100") && near(s.diffuse_white_nits,100) && !set(s,"diffuse_white_nits","79") && !set(s,"diffuse_white_nits","401"));
    CHECK(set(s,"style","2") && s.style==2 && !set(s,"style","3"));
    CHECK(set(s,"intensity","0.5") && near(s.intensity,.5) && !set(s,"intensity","1.5") && !set(s,"intensity","-0.1"));
    CHECK(!set(s,"no_such_key","1"));
    // A refused value names its key.
    { std::string error; CHECK(!set_dlss5_field(s,"passes","9",error) && error.find("passes")!=std::string::npos); }
    { std::string error; CHECK(!set_dlss5_field(s,"bogus","9",error) && error.find("bogus")!=std::string::npos); }
    // Names round-trip through the text form.
    for(const auto& f:dlss5_fields()) {
        Dlss5Settings a; f.set(a,f.max);
        Dlss5Settings b; CHECK(set(b,f.key,dlss5_field_text(f,a)) && b==a);
    }
    // The NGX preset hint is a fixed table, distinct per preset.
    std::set<int> hints; for(const auto p:{Dlss5Preset::p1,Dlss5Preset::p2,Dlss5Preset::p3,Dlss5Preset::h,Dlss5Preset::a,Dlss5Preset::b}) hints.insert(dlss5_preset_hint(p));
    CHECK(hints.size()==6);
    // The tab's controls: the switch is usable unless the GPU cannot run it (a saved "on" can always be switched off); the rest follows the switch.
    auto ui=dlss5_ui(Dlss5Settings{},false,false);
    CHECK(ui.switch_enabled && !ui.settings_enabled);
    ui=dlss5_ui(Dlss5Settings{},true,false);
    CHECK(!ui.switch_enabled && !ui.settings_enabled);
    s=Dlss5Settings{}; s.enabled=true;
    ui=dlss5_ui(s,true,false); CHECK(ui.switch_enabled && !ui.settings_enabled);
    ui=dlss5_ui(s,false,false); CHECK(ui.switch_enabled && ui.settings_enabled);
    ui=dlss5_ui(s,true,true); CHECK(ui.switch_enabled && ui.settings_enabled);
    ui=dlss5_ui(Dlss5Settings{},true,true); CHECK(ui.switch_enabled && !ui.settings_enabled);
    // The status line: the run's own reason wins; before the report arrives it is "checking"; a GPU the probe refused shows the probe's reason
    { const auto st=dlss5_status(false,false,"","");  CHECK(st.state==Dlss5State::checking && st.reason.empty()); }
    { const auto st=dlss5_status(true,true,"","");    CHECK(st.state==Dlss5State::available && st.reason.empty()); }
    { const auto st=dlss5_status(true,false,"too old",""); CHECK(st.state==Dlss5State::unavailable && st.reason=="too old"); }
    { const auto st=dlss5_status(true,true,"","create failed"); CHECK(st.state==Dlss5State::unavailable && st.reason=="create failed"); }
    { const auto st=dlss5_status(false,false,"","missing"); CHECK(st.state==Dlss5State::unavailable && st.reason=="missing"); }
    // The run condition: switched on, and the feature could be created.
    CHECK(!dlss5_wanted(Dlss5Settings{},true) && !dlss5_wanted(s,false) && dlss5_wanted(s,true));
    // Bridge math: signal encodings decode and encode each other, the gain composes, the processing size is even and never empty.
    for(const auto e:{Dlss5Encoding::automatic,Dlss5Encoding::srgb,Dlss5Encoding::gamma22,Dlss5Encoding::gamma24,Dlss5Encoding::linear})
        for(const float v:{0.f,.02f,.18f,.5f,.9f,1.f}) CHECK(near(dlss5_encode(e,dlss5_decode(e,v)),v,1e-5));
    CHECK(near(dlss5_decode(Dlss5Encoding::srgb,.5f),.21404114,1e-6) && near(dlss5_decode(Dlss5Encoding::automatic,.5f),.21404114,1e-6));
    CHECK(near(dlss5_decode(Dlss5Encoding::gamma22,.5f),std::pow(.5,2.2),1e-6) && near(dlss5_decode(Dlss5Encoding::gamma24,.5f),std::pow(.5,2.4),1e-6) && near(dlss5_decode(Dlss5Encoding::linear,.5f),.5));
    Dlss5Settings g; CHECK(near(dlss5_gain(g),1));
    g.exposure=2; CHECK(near(dlss5_gain(g),1));   // auto mode ignores the value
    g.exposure_mode=Dlss5ExposureMode::manual; CHECK(near(dlss5_gain(g),2));
    g.diffuse_white_nits=406; CHECK(near(dlss5_gain(g),2));   // no override: the nits value is unused
    g.diffuse_white_override=true; CHECK(near(dlss5_gain(g),4));
    g.exposure_mode=Dlss5ExposureMode::automatic; CHECK(near(dlss5_gain(g),2));
    auto e=dlss5_processing_extent(1280,720,1.0); CHECK(e.width==1280 && e.height==720);
    e=dlss5_processing_extent(1280,720,.5); CHECK(e.width==640 && e.height==360);
    e=dlss5_processing_extent(1283,721,.75); CHECK(e.width%2==0 && e.height%2==0 && e.width==962 && e.height==540);
    e=dlss5_processing_extent(10,10,.5); CHECK(e.width>=8 && e.height>=8);
    return 0;
}
