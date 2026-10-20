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
    // The table: unique keys, every default inside its own range, getters and setters agree, booleans carry Off | On, choices span their range.
    std::set<std::string_view> keys;
    const Dlss5Settings defaults;
    for(const auto& f:dlss5_fields()) {
        CHECK(keys.insert(f.key).second);
        CHECK(dlss5_value_error(f,f.get(defaults)).empty());
        CHECK(dlss5_field_default(f)==f.get(defaults));
        Dlss5Settings copy; f.set(copy,f.get(defaults)); CHECK(copy==defaults);
        if(f.kind==Dlss5Kind::choice) { CHECK(!f.choices.empty() && f.max==double(f.choices.size()-1) && f.min==0); }
        if(f.kind==Dlss5Kind::boolean) { CHECK(f.choices.size()==2 && f.max==1 && f.min==0); }
        if(f.kind==Dlss5Kind::real || f.kind==Dlss5Kind::integer) { CHECK(f.choices.empty() && f.control==Dlss5Control::slider); }
        if(f.control==Dlss5Control::bar || f.control==Dlss5Control::combo) CHECK(!f.choices.empty());
        CHECK(f.visible);
    }
    CHECK(keys.size()==22);
    {   // the order of the tab: Preset first, then the Scaling and Advanced rows after the main ones
        const auto fields=dlss5_fields();
        CHECK(fields[0].key=="preset" && !fields[0].resettable && fields[1].key=="hook" && fields[21].key=="exposure_value");
        Dlss5Section last=Dlss5Section::main;
        for(const auto& f:fields) { CHECK(int(f.section)>=int(last)); last=f.section; if(f.key!="preset") CHECK(f.resettable); }
        CHECK(dlss5_field("resolution_scale")->section==Dlss5Section::scaling && dlss5_field("passes")->section==Dlss5Section::advanced && dlss5_field("tone")->section==Dlss5Section::main);
    }
    // The owner's defaults: the first option of every list; sliders 1.00 / 100% / 1 pass / 500 nits / 1.00x
    CHECK(defaults.preset==Dlss5Preset::p1 && defaults.hook==Dlss5Hook::off && !defaults.enabled() && defaults.ui_correction==Dlss5UiCorrection::automatic && defaults.model==Dlss5Model::a && !defaults.character_mask);
    CHECK(defaults.scaling_domain==Dlss5Domain::scrgb_nl && defaults.downsample_filter==Dlss5DownFilter::area && defaults.upsample_filter==Dlss5UpFilter::bilinear && defaults.scale_reconstruction==Dlss5Reconstruction::rgb_residual);
    CHECK(defaults.render_preset==Dlss5RenderPreset::automatic && defaults.encoding==Dlss5Encoding::automatic && defaults.nr_encoding==Dlss5NrEncoding::srgb && !defaults.block_hudless && defaults.exposure_value==Dlss5ExposureValue::off);
    CHECK(defaults.intensity==1 && defaults.structure==1 && defaults.tone==1 && defaults.character_intensity==1 && defaults.resolution_scale==1 && defaults.passes==1 && defaults.diffuse_white_nits==500 && defaults.exposure==1);
    // Parsing is strict: whole text, type and range.
    Dlss5Settings s;
    CHECK(set(s,"character_mask","true") && s.character_mask && set(s,"character_mask","false") && !s.character_mask);
    CHECK(!set(s,"character_mask","1") && !set(s,"character_mask","") && !set(s,"character_mask","True"));
    CHECK(set(s,"passes","3") && s.passes==3);
    CHECK(!set(s,"passes","0") && !set(s,"passes","5") && !set(s,"passes","2x") && !set(s,"passes","-1") && !set(s,"passes","2.0") && s.passes==3);
    CHECK(set(s,"resolution_scale","0.75") && near(s.resolution_scale,.75));
    CHECK(!set(s,"resolution_scale","0.4") && !set(s,"resolution_scale","1.01") && !set(s,"resolution_scale","nan") && !set(s,"resolution_scale","inf") && !set(s,"resolution_scale","0.5x") && near(s.resolution_scale,.75));
    CHECK(set(s,"preset","h") && s.preset==Dlss5Preset::h && !set(s,"preset","a") && !set(s,"preset","H") && !set(s,"preset","") && s.preset==Dlss5Preset::h);
    CHECK(set(s,"hook","present") && s.hook==Dlss5Hook::present && s.enabled() && set(s,"hook","render") && s.hook==Dlss5Hook::render && set(s,"hook","off") && !s.enabled() && !set(s,"hook","hud"));
    CHECK(set(s,"encoding","hdr10") && s.encoding==Dlss5Encoding::hdr10 && set(s,"encoding","auto") && s.encoding==Dlss5Encoding::automatic && !set(s,"encoding","bt2020"));
    CHECK(set(s,"nr_encoding","pq_sdr") && s.nr_encoding==Dlss5NrEncoding::pq_sdr && set(s,"nr_encoding","linear_sdr") && s.nr_encoding==Dlss5NrEncoding::linear_sdr);
    CHECK(set(s,"downsample_filter","nearest") && s.downsample_filter==Dlss5DownFilter::nearest && !set(s,"downsample_filter","bicubic") && set(s,"upsample_filter","bicubic") && s.upsample_filter==Dlss5UpFilter::bicubic);
    CHECK(set(s,"scaling_domain","signed_pq") && s.scaling_domain==Dlss5Domain::signed_pq && set(s,"scale_reconstruction","processed") && s.scale_reconstruction==Dlss5Reconstruction::processed);
    CHECK(set(s,"ui_correction","on") && s.ui_correction==Dlss5UiCorrection::on && set(s,"model","c") && s.model==Dlss5Model::c && set(s,"render_preset","b") && s.render_preset==Dlss5RenderPreset::b);
    CHECK(set(s,"exposure_value","inverse") && s.exposure_value==Dlss5ExposureValue::inverse && set(s,"exposure","2") && near(s.exposure,2) && !set(s,"exposure","0.1") && !set(s,"exposure","8"));
    CHECK(set(s,"diffuse_white_nits","1000") && near(s.diffuse_white_nits,1000) && !set(s,"diffuse_white_nits","79") && !set(s,"diffuse_white_nits","1001"));
    CHECK(set(s,"intensity","1.5") && near(s.intensity,1.5) && !set(s,"intensity","2.5") && !set(s,"intensity","-0.1"));
    CHECK(!set(s,"enabled","true") && !set(s,"stage","present") && !set(s,"no_such_key","1"));   // the old keys are gone
    { std::string error; CHECK(!set_dlss5_field(s,"passes","9",error) && error.find("passes")!=std::string::npos); }
    { std::string error; CHECK(!set_dlss5_field(s,"bogus","9",error) && error.find("bogus")!=std::string::npos); }
    // Names round-trip through the text form.
    for(const auto& f:dlss5_fields()) {
        Dlss5Settings a; f.set(a,f.max);
        Dlss5Settings b; CHECK(set(b,f.key,dlss5_field_text(f,a)) && b==a);
    }
    // The NGX preset hint is a fixed table: Advanced "default" sends the top preset, A and B win over it
    { Dlss5Settings h; std::set<int> seen;
      for(const auto p:{Dlss5Preset::p1,Dlss5Preset::p2,Dlss5Preset::p3,Dlss5Preset::h}) { h.preset=p; seen.insert(dlss5_preset_hint(h)); }
      h.render_preset=Dlss5RenderPreset::a; seen.insert(dlss5_preset_hint(h)); h.render_preset=Dlss5RenderPreset::b; seen.insert(dlss5_preset_hint(h));
      CHECK(seen.size()==6); h.preset=Dlss5Preset::p3; CHECK(dlss5_preset_hint(h)==6); h.render_preset=Dlss5RenderPreset::automatic; CHECK(dlss5_preset_hint(h)==3); }
    // The tab's controls: the Hook Method bar is usable unless the GPU cannot run it (a saved hook can always be set to Off); the other rows follow only the GPU
    auto ui=dlss5_ui(Dlss5Settings{},false,false);
    CHECK(ui.switch_enabled && ui.settings_enabled);
    ui=dlss5_ui(Dlss5Settings{},true,false);
    CHECK(!ui.switch_enabled && !ui.settings_enabled);
    s=Dlss5Settings{}; s.hook=Dlss5Hook::upscaled;
    ui=dlss5_ui(s,true,false); CHECK(ui.switch_enabled && !ui.settings_enabled);
    ui=dlss5_ui(s,true,true); CHECK(ui.switch_enabled && ui.settings_enabled);
    ui=dlss5_ui(Dlss5Settings{},true,true); CHECK(ui.switch_enabled && ui.settings_enabled);
    // The status line: the run's own reason wins; before the report arrives it is "checking"; a GPU the probe refused shows the probe's reason
    { const auto st=dlss5_status(false,false,"","");  CHECK(st.state==Dlss5State::checking && st.reason.empty()); }
    { const auto st=dlss5_status(true,true,"","");    CHECK(st.state==Dlss5State::available && st.reason.empty()); }
    { const auto st=dlss5_status(true,false,"too old",""); CHECK(st.state==Dlss5State::unavailable && st.reason=="too old"); }
    { const auto st=dlss5_status(true,true,"","create failed"); CHECK(st.state==Dlss5State::unavailable && st.reason=="create failed"); }
    { const auto st=dlss5_status(false,false,"","missing"); CHECK(st.state==Dlss5State::unavailable && st.reason=="missing"); }
    // The run condition: a hook is chosen, and the feature could be created.
    CHECK(!dlss5_wanted(Dlss5Settings{},true) && !dlss5_wanted(s,false) && dlss5_wanted(s,true));
    // Bridge math: every encoding decodes and encodes back, PQ and sRGB have their known values, the sign survives
    CHECK(near(dlss5_srgb_eotf(.5f),.21404114,1e-6) && near(dlss5_srgb_oetf(.21404114f),.5,1e-5) && near(dlss5_pq_oetf(100.f),.5081,2e-4) && near(dlss5_pq_eotf(dlss5_pq_oetf(100.f)),100,.05));
    CHECK(near(dlss5_srgb_eotf(-.5f),-.21404114,1e-6) && near(dlss5_srgb_oetf(-.21404114f),-.5,1e-5));
    Dlss5Settings m; m.diffuse_white_nits=500;
    for(const auto e:{Dlss5Encoding::automatic,Dlss5Encoding::linear709,Dlss5Encoding::srgb,Dlss5Encoding::hdr10,Dlss5Encoding::scrgb,Dlss5Encoding::scrgb_nl}) {
        m.encoding=e;
        for(const float v:{0.f,.02f,.18f,.5f,.9f,1.f}) CHECK(near(dlss5_encode_source(m,dlss5_decode_source(m,v)),v,2e-4));
    }
    m.encoding=Dlss5Encoding::automatic; CHECK(near(dlss5_decode_source(m,.5f),.21404114,1e-6));
    m.encoding=Dlss5Encoding::scrgb; CHECK(near(dlss5_decode_source(m,.5f),.5));
    m.encoding=Dlss5Encoding::hdr10; CHECK(near(dlss5_decode_source(m,dlss5_pq_oetf(250.f)),.5,1e-3));   // 250 nits at 500 nits diffuse white
    for(const auto n:{Dlss5NrEncoding::srgb,Dlss5NrEncoding::pq_sdr,Dlss5NrEncoding::linear_sdr}) {
        m.nr_encoding=n;
        for(const float l:{-.3f,0.f,.02f,.18f,.5f,1.f,2.f}) CHECK(near(dlss5_nr_decode(m,dlss5_nr_encode(m,l)),l,2e-3));
    }
    for(const auto d:{Dlss5Domain::scrgb_nl,Dlss5Domain::signed_pq,Dlss5Domain::linear}) {
        m.scaling_domain=d;
        for(const float l:{-.3f,0.f,.02f,.18f,.5f,1.f,2.f}) CHECK(near(dlss5_domain_from(m,dlss5_domain_to(m,l)),l,2e-3));
    }
    Dlss5Settings g; g.exposure=2;
    CHECK(near(dlss5_pre_gain(g),1));   // Use Exposure Value off
    g.exposure_value=Dlss5ExposureValue::on; CHECK(near(dlss5_pre_gain(g),2));
    g.exposure_value=Dlss5ExposureValue::inverse; CHECK(near(dlss5_pre_gain(g),.5));
    auto e=dlss5_processing_extent(1280,720,1.0); CHECK(e.width==1280 && e.height==720);
    e=dlss5_processing_extent(1280,720,.5); CHECK(e.width==640 && e.height==360);
    e=dlss5_processing_extent(1283,721,.75); CHECK(e.width%2==0 && e.height%2==0 && e.width==962 && e.height==540);
    e=dlss5_processing_extent(10,10,.5); CHECK(e.width>=8 && e.height>=8);
    return 0;
}
