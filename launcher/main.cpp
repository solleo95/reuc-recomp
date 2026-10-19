#include "binding_editor.h"
#include "launch.h"
#include "cpu_calibration.h"
#include "options_config.h"
#include "feature_report.h"
#include "render_scale.h"
#include "prompt_icons.h"
#include "localization.h"
#include "graphite.h"
#include "ui_fonts.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <future>
#include <memory>
#include <thread>
#include <map>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
namespace fs=std::filesystem;
namespace {
std::string utf8(const fs::path& path) {auto value=path.u8string();return {value.begin(),value.end()};}
fs::path executable_dir() {
    std::wstring value(32768,L'\0');const auto length=GetModuleFileNameW(nullptr,value.data(),DWORD(value.size()));
    if(!length || length>=value.size())throw std::runtime_error("cannot locate launcher executable");
    value.resize(length);return fs::path(value).parent_path();
}
struct Ui {
    SDL_Window* window=nullptr;SDL_Renderer* renderer=nullptr;
    bool context=false,platform=false,backend=false;
    uc::ui::Fonts fonts;
    SDL_Texture* banner=nullptr;
    ImFont* sans=nullptr;
    ImFont* serif=nullptr;
    std::string artwork_error;
    ~Ui() {
        if(backend)ImGui_ImplSDLRenderer3_Shutdown();
        if(platform)ImGui_ImplSDL3_Shutdown();
        if(context)ImGui::DestroyContext();
        SDL_DestroyTexture(banner);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
    }
    void init(bool diagnostic) {
        if(!SDL_Init(SDL_INIT_VIDEO))throw std::runtime_error(SDL_GetError());
        window=SDL_CreateWindow("Resident Evil: The Umbrella Chronicles",1200,820,SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIDDEN|SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if(!window)throw std::runtime_error(SDL_GetError());
        SDL_SetWindowMinimumSize(window,1000,720);
        const float dpi=std::max(1.f,SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
        SDL_Rect bounds{};
        if(SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(),&bounds)) {
            const float scale=std::min({1200*dpi/1504,(bounds.w-48.f)/1504,(bounds.h-64.f)/916});
            SDL_SetWindowSize(window,int(1504*scale),int(916*scale));
        }
        auto hwnd=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
        if(hwnd) {BOOL dark=TRUE;DwmSetWindowAttribute(hwnd,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));}
        renderer=SDL_CreateRenderer(window,nullptr);
        if(!renderer || !SDL_SetRenderVSync(renderer,1))throw std::runtime_error(SDL_GetError());
        ImGui::CreateContext();context=true;
        auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
        if(diagnostic)io.ConfigInputTrickleEventQueue=false;
        uc::launcher::graphite_style();
        ImGui::GetStyle().FontScaleDpi=1;
        fonts=uc::ui::load_fonts(*io.Fonts);sans=fonts.sans;serif=fonts.serif;
        platform=ImGui_ImplSDL3_InitForSDLRenderer(window,renderer);
        backend=ImGui_ImplSDLRenderer3_Init(renderer);
        if(!platform || !backend)throw std::runtime_error("ImGui backend initialization failed");
        try {banner=uc::launcher::load_banner(renderer,executable_dir()/L"assets"/L"launcher"/L"graphite-atlas.png");}
        catch(const std::exception& e) {artwork_error=e.what();std::fprintf(stderr,"launcher artwork: %s\n",e.what());}
        if(diagnostic) {
            auto hwnd=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
            if(!hwnd)throw std::runtime_error("launcher window handle unavailable");
            if(GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(hwnd))!=DPI_AWARENESS_PER_MONITOR_AWARE)
                throw std::runtime_error("launcher is not per-monitor DPI aware");
            int pixel_width=0,pixel_height=0;
            if(!SDL_GetWindowSizeInPixels(window,&pixel_width,&pixel_height))throw std::runtime_error(SDL_GetError());
            std::fprintf(stderr,"launcher DPI=%u content-scale=%.2f framebuffer=%dx%d\n",GetDpiForWindow(hwnd),SDL_GetWindowDisplayScale(window),pixel_width,pixel_height);
            ShowWindow(hwnd,SW_SHOWNOACTIVATE);
        } else if(!SDL_ShowWindow(window))throw std::runtime_error(SDL_GetError());
    }
    void capture(const fs::path& path) {
        auto* surface=SDL_RenderReadPixels(renderer,nullptr);
        if(!surface)throw std::runtime_error(SDL_GetError());
        const bool saved=SDL_SaveBMP(surface,utf8(path).c_str());SDL_DestroySurface(surface);
        if(!saved)throw std::runtime_error(SDL_GetError());
    }
    void art(ImDrawList& draw,ImVec2 at,ImVec2 size,ImVec2 source,ImVec2 end) {
        if(banner)draw.AddImage((ImTextureID)(intptr_t)banner,at,{at.x+size.x,at.y+size.y},
            {source.x/banner->w,source.y/banner->h},{end.x/banner->w,end.y/banner->h});
    }
};
std::string active_language="en";
const char* tr(std::string_view key){return uc::launcher::text(active_language,key).data();}
std::string physical_name(uc::PhysicalInput input) {
    if(input.kind==uc::PhysicalKind::key) {
        const auto* name=SDL_GetScancodeName(SDL_Scancode(input.code));
        return name && *name ? name : std::string(tr("key"))+" "+std::to_string(input.code);
    }
    if(input.kind==uc::PhysicalKind::wheel)return tr(input.code==1 ? "wheel_up" : "wheel_down");
    if(input.code==1)return tr("lmb");if(input.code==3)return tr("rmb");if(input.code==2)return tr("mmb");
    return std::string(tr("mouse"))+" "+std::to_string(input.code);
}
std::string binding_name(const uc::Binding& binding) {
    if(binding.kind==uc::BindingKind::interact_fire)return tr("interact_fire");
    if(binding.kind==uc::BindingKind::interact_motion)return tr("interact_motion");
    if(binding.kind==uc::BindingKind::qte_motion)return tr("rapid");
    std::string text;for(auto modifier:binding.modifiers)text+=physical_name(modifier)+" + ";
    return text+physical_name(binding.input);
}
bool same_options(const uc::Options& a,const uc::Options& b) {
    return a.game==b.game && a.render_scale==b.render_scale && a.window_mode==b.window_mode && a.width==b.width && a.height==b.height && a.skip_intro==b.skip_intro && a.skip_tutorials==b.skip_tutorials && a.fps_limit==b.fps_limit && a.prompt_style==b.prompt_style && a.bloom==b.bloom
        && a.upscaler==b.upscaler && a.upscale_mode==b.upscale_mode && a.sharpening==b.sharpening && a.sharpness==b.sharpness
        && a.frame_generation==b.frame_generation && a.frame_generation_multiplier==b.frame_generation_multiplier && a.effects==b.effects;
}
int run(int argc,wchar_t** argv) {
    const auto directory=executable_dir();fs::path config_path=directory/L"config.json",smoke,smoke_exe;
    for(int i=1;i<argc;++i) {
        const std::wstring_view arg=argv[i];
        if((arg==L"--config" || arg==L"--smoke" || arg==L"--smoke-exe") && i+1<argc) {
            if(arg==L"--config")config_path=argv[++i];else if(arg==L"--smoke")smoke=argv[++i];else smoke_exe=argv[++i];
        } else throw std::invalid_argument("usage: uc_launcher [--config path] [--smoke directory]");
    }
    if(!smoke.empty()) {fs::create_directories(smoke);config_path=smoke/L"config.json";}
    if(!smoke_exe.empty() && smoke.empty())throw std::invalid_argument("--smoke-exe requires --smoke");
    std::unique_ptr<uc::CpuCalibration> calibration;
    uc::CpuCalibrationResult cpu_choice;bool cpu_ready=false,launch_requested=false;
    try {calibration=std::make_unique<uc::CpuCalibration>((smoke.empty() ? directory : smoke)/L"cpu-profile.json");}
    catch(const std::exception& e) {cpu_ready=true;std::fprintf(stderr,"[cpu-calibration] Windows scheduling: %s\n",e.what());}
    uc::Options diagnostic;
    if(!smoke_exe.empty()) {
        diagnostic.test_control=fs::absolute(smoke/L"control.txt");diagnostic.save_root=fs::absolute(smoke/L"saves");
        fs::copy(L"S:/Games/Umbrella Chronicles/saves",diagnostic.save_root,fs::copy_options::recursive|fs::copy_options::skip_existing);
    }
    uc::ConfigDocument document;uc::Options options,saved_options;uc::BindingEditor editor;uc::GameProcess process;
    std::string error;bool blocked=false;
    const auto system_language=uc::launcher::system_language();
    std::string language="system",saved_language="system";
    active_language=system_language;
    bool failed=false;uc::Options failed_options;uc::Bindings failed_bindings;std::string failed_language;   // the last state an automatic save was refused for
    auto reload=[&] {
        failed=false;
        try {auto loaded=uc::load_config(config_path);auto settings=uc::options_from_config(loaded);
            auto locale=uc::launcher::config_language(loaded);
            editor=uc::BindingEditor(loaded.input);document=std::move(loaded);options=saved_options=settings;
            language=saved_language=locale;active_language=locale=="system" ? system_language : locale;blocked=false;error.clear();
        } catch(const std::exception& e) {error=e.what();blocked=true;}
    };
    reload();std::string game_path=utf8(options.game);
    // What DLSS, FSR and frame generation this PC supports, asked of uc.exe itself (the same runtime files the game loads) while the window opens.
    uc::FeatureReport features;std::future<std::string> features_query;bool features_ready=false;
    struct DetachedQuery {std::shared_ptr<std::promise<std::string>> promise;std::thread thread;~DetachedQuery() {if(thread.joinable())thread.detach();}} query_thread;   // closing the launcher must not wait for a hung query
    if(!smoke.empty()) {features.dlss=features.dlss_g=features.fsr=features.fsr_fg=true;features.dlss_g_max_multiplier=4;features.effects=true;features.anisotropy_max=16;features_ready=true;}   // the smoke run exercises the controls whatever this PC has
    if(smoke.empty() && fs::is_regular_file(directory/L"uc.exe")) {
        query_thread.promise=std::make_shared<std::promise<std::string>>();features_query=query_thread.promise->get_future();
        query_thread.thread=std::thread([promise=query_thread.promise,exe=directory/L"uc.exe",config=config_path] {promise->set_value(uc::query_features(exe,config));});
    }
    Ui ui;ui.init(!smoke.empty());
    const auto* monitor=SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if(!monitor || monitor->w<=0 || monitor->h<=0)throw std::runtime_error("monitor mode unavailable");
    std::map<std::string,ImVec2> positions;
    auto track=[&](const std::string& name) {if(!smoke.empty()) {auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();positions[name]={(a.x+b.x)/2,(a.y+b.y)/2};
        if((name=="launch" || name=="browse") && (a.x<0 || a.y<0 || b.x>ImGui::GetIO().DisplaySize.x || b.y>ImGui::GetIO().DisplaySize.y))throw std::runtime_error("launcher footer clipped: "+name+" bottom="+std::to_string(b.y)+" height="+std::to_string(ImGui::GetIO().DisplaySize.y));
    }};
    auto button=[&](const char* label,const std::string& id,ImVec2 size=ImVec2{0,0}) {ImGui::PushID(id.c_str());bool clicked=ImGui::Button(label,size);track(id);ImGui::PopID();return clicked;};
    auto save=[&](const uc::Options& chosen) {
        try {auto next=document;next.input=editor.bindings();uc::store_options(next,chosen);uc::launcher::store_language(next,language);uc::save_config(config_path,next);
            document=std::move(next);saved_options=chosen;saved_language=language;error.clear();failed=false;return true;
        } catch(const std::exception& e) {error=e.what();return false;}
    };
    bool done=false,smoke_capture_wait_checked=false;unsigned frame=0;const auto start=SDL_GetTicks();int page=1;
    while(!done) {
        SDL_Event event;
        while(SDL_PollEvent(&event)) {
            if(event.type==SDL_EVENT_QUIT || (event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID==SDL_GetWindowID(ui.window)))done=true;
            if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST)editor.focus(false);
            if(smoke.empty()) {
                if(editor.capturing()) {
                    if((event.type==SDL_EVENT_KEY_DOWN || event.type==SDL_EVENT_KEY_UP) && !event.key.repeat)editor.event({uc::PhysicalKind::key,uint16_t(event.key.scancode)},event.key.down);
                    else if(event.type==SDL_EVENT_MOUSE_BUTTON_DOWN || event.type==SDL_EVENT_MOUSE_BUTTON_UP)editor.event({uc::PhysicalKind::mouse,event.button.button},event.button.down);
                    else if(event.type==SDL_EVENT_MOUSE_WHEEL)editor.wheel(event.wheel.y==0 ? 0 : ((event.wheel.y>0) != (event.wheel.direction==SDL_MOUSEWHEEL_FLIPPED) ? 1 : -1));
                } else ImGui_ImplSDL3_ProcessEvent(&event);
            }
        }
        if(calibration && calibration->ready()) {cpu_choice=calibration->get();calibration.reset();cpu_ready=true;}
        if(features_query.valid() && features_query.wait_for(std::chrono::seconds(0))==std::future_status::ready) {features=uc::feature_report_from_json(features_query.get());features_ready=true;}
        ImGui_ImplSDLRenderer3_NewFrame();ImGui_ImplSDL3_NewFrame();
        if(!smoke.empty()) {
            auto& io=ImGui::GetIO();io.AddFocusEvent(true);
            io.AddMousePosEvent(-100,-100);
            std::map<unsigned,std::string> clicks={{10,"controls"},{18,"clear1"},{26,"add1"},{48,"clear1"},
                {80,"controls"},{84,"style"},{88,"style3"},{98,"style"},{102,"style2"},{110,"general"},
                {366,"game"},{374,"audio"},{382,"controls"},{390,"general"},
                {400,"window"},{404,"mode1"},{410,"scale"},{414,"scale1.5"},{418,"fps"},{422,"fps144"},{426,"framegen"},{430,"framegen-dlss"},
                {434,"framegen"},{438,"framegen-off"},{442,"framegen"},{446,"framegen-fsr"},{450,"framegen"},{454,"framegen-off"},
                {458,"upscaling"},{462,"upscaler-dlss"},{470,"controls"},{476,"replace2"}};
            if(frame==360 && !SDL_SetWindowSize(ui.window,1000,720))throw std::runtime_error(SDL_GetError());
            for(size_t i=0;i<uc::launcher::languages().size();++i) {
                unsigned first=114+unsigned(i)*18;
                clicks[first]="language";clicks[first+4]="lang-"+std::string(uc::launcher::languages()[i].code);
            }
            if(smoke_exe.empty()) {clicks[56]="reload";clicks[62]="defaults";}
            else {clicks[4]="launch";clicks[56]="reload";clicks[62]="defaults";}
            for(auto [when,id]:clicks)if(frame>=when-2 && frame<=when+1) {
                if(!positions.contains(id))throw std::runtime_error("launcher smoke widget missing: "+id);
                const auto point=positions.at(id);io.AddMousePosEvent(point.x,point.y);
                if(frame==when || frame==when+1)io.AddMouseButtonEvent(0,frame==when);
            }
            if(frame==32) {if(!editor.capturing())throw std::runtime_error("capture UI did not start");editor.event({uc::PhysicalKind::key,41},true);}
            if(frame==480) {if(editor.capturing()!=uc::Action::weapon_previous)throw std::runtime_error("grid replacement capture did not start");editor.event({uc::PhysicalKind::key,26},true);}
            if(frame==44) {auto other=uc::load_config(config_path);other.properties["smoke_external_edit"]=!other.properties.value("smoke_external_edit",false);uc::save_config(config_path,other);}
        }
        ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        if(!smoke.empty() && frame==2) {
            for(const auto& [code,name]:std::map<uint16_t,std::string>{{26,"W"},{22,"S"},{21,"R"},{10,"G"},{25,"V"},{41,"Escape"}})
                if(physical_name({uc::PhysicalKind::key,code})!=name)throw std::runtime_error("physical key label mismatch: "+name);
            for(auto* font:{ui.sans,ui.serif}) {
            auto* baked=font->GetFontBaked(22);
            for(auto locale:uc::launcher::languages())for(auto key:uc::launcher::translation_keys()) {
                auto value=uc::launcher::text(locale.code,key);const auto length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0);
                if(length<=0)throw std::runtime_error("invalid UTF-8 translation");
                std::wstring wide(length,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),wide.data(),length);
                for(auto character:wide)if(character>32 && !baked->FindGlyphNoFallback(character))throw std::runtime_error("missing launcher glyph: "+std::string(locale.code)+" "+std::to_string(unsigned(character)));
            }
            }
        }
        const float unit=std::max(.60f,std::min(ImGui::GetIO().DisplaySize.x/1504.f,ImGui::GetIO().DisplaySize.y/916.f));
        uc::launcher::graphite_style();ImGui::GetStyle().ScaleAllSizes(unit);
        // ScaleAllSizes floors thin borders to zero below 1×.
        ImGui::GetStyle().FrameBorderSize=ImGui::GetStyle().PopupBorderSize=1;
        auto push_font=[&](ImFont* font,float size){ImGui::PushFont(font,std::round(size*unit));};
        push_font(ui.sans,22);
        ImGui::Begin("Launcher",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollbar);
        const auto available=ImGui::GetContentRegionAvail();
        const float sidebar=std::round(344*unit);
        auto combo=[&](const char* id,const char* preview,ImGuiComboFlags flags=0) {
            const auto expected_origin=ImGui::GetCursorScreenPos();
            auto* list=ImGui::GetWindowDrawList();auto* font=ImGui::GetFont();const float size=ImGui::GetFontSize();
            const auto color=ImGui::GetColorU32(ImGuiCol_Text);
            // BeginCombo switches to the popup window; retain the parent frame geometry.
            const auto a=expected_origin;
            const ImVec2 b{a.x+ImGui::CalcItemWidth(),a.y+ImGui::GetFrameHeight()};
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{12*unit,8*unit});
            const bool opened=ImGui::BeginCombo(id,"",flags|ImGuiComboFlags_NoArrowButton);
            ImGui::PopStyleVar();
            if(!smoke.empty() && opened && (a.x!=expected_origin.x || a.y!=expected_origin.y))
                throw std::runtime_error(std::string("combo anchor changed while opening: ")+id);
            const ImVec4 clip{a.x+14*unit,a.y,b.x-36*unit,b.y};
            list->AddRectFilledMultiColor({a.x+2,a.y+2},{b.x-2,b.y-2},IM_COL32(255,255,255,5),IM_COL32(255,255,255,5),0,0);
            list->AddText(font,size,{std::round(a.x+16*unit),std::round(a.y+(b.y-a.y-size)/2)},color,preview,nullptr,0,&clip);
            const float x=b.x-25*unit,y=(a.y+b.y)/2;
            list->AddLine({x-6*unit,y-3*unit},{x,y+3*unit},color,unit);
            list->AddLine({x,y+3*unit},{x+6*unit,y-3*unit},color,unit);
            return opened;
        };
        auto* background=ImGui::GetWindowDrawList();
        background->AddRectFilledMultiColor({0,0},{sidebar,available.y},IM_COL32(12,16,17,255),IM_COL32(15,19,20,255),IM_COL32(12,16,17,255),IM_COL32(9,13,14,255));
        background->AddRectFilledMultiColor({sidebar,0},available,IM_COL32(25,29,30,255),IM_COL32(19,23,24,255),IM_COL32(15,19,20,255),IM_COL32(22,26,27,255));
        ImGui::BeginChild("sidebar",{sidebar,0},ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar);
        auto* draw=ImGui::GetWindowDrawList();
        if(ui.banner)ui.art(*draw,{0,0},{sidebar,std::round(446*unit)},{17,75},{359,521});
        else {uc::launcher::draw_umbrella(*draw,{sidebar/2,200*unit},90*unit);draw->AddText({28*unit,50*unit},IM_COL32_WHITE,"RESIDENT EVIL");}
        const char* sections[]={"game","graphics","controls","audio"};
        ImGui::SetCursorPos({6*unit,455*unit});
        for(int i=0;i<4;++i) {
            ImGui::SetCursorPosX(std::round(6*unit));
            ImGui::PushID(i);auto p=ImGui::GetCursorScreenPos();
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
            ImGui::PushStyleColor(ImGuiCol_Button,page==i ? ImVec4{.34f,.055f,.075f,1} : ImVec4{0,0,0,0});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,{.30f,.07f,.09f,1});
            if(button("##navigation",i==1 ? "general" : sections[i],{sidebar-12*unit,60*unit}))page=i;
            ImGui::PopStyleColor(2);ImGui::PopStyleVar();
            const auto color=page==i ? IM_COL32(246,243,237,255) : IM_COL32(161,164,164,255);
            if(page==i)draw->AddRectFilled(p,{p.x+5*unit,p.y+60*unit},IM_COL32(200,33,47,255),2*unit);
            uc::launcher::draw_navigation_icon(*draw,i,{std::round(p.x+32*unit),std::round(p.y+15*unit)},30*unit,color);
            draw->AddText(ui.sans,std::round(24*unit),{std::round(p.x+89*unit),std::round(p.y+16*unit)},color,tr(sections[i]));
            ImGui::PopID();
        }
        ImGui::SetCursorPos({38*unit,std::max(ImGui::GetCursorPosY()+16*unit,available.y-150*unit)});
        ImGui::PushFont(ui.sans,std::round(22*unit));ImGui::TextDisabled("%s",tr("language"));
        const char* language_label=tr("system");
        for(auto item:uc::launcher::languages())if(item.code==language)language_label=item.name.data();
        ImGui::SetCursorPosX(38*unit);
        ImGui::SetNextItemWidth(sidebar-76*unit);
        const bool languages_open=combo("##language",language_label,ImGuiComboFlags_HeightLargest);track("language");
        if(languages_open) {
            if(ImGui::Selectable(tr("system"),language=="system"))language="system";track("lang-system");
            for(auto item:uc::launcher::languages()) {
                if(ImGui::Selectable(item.name.data(),language==item.code))language=item.code;
                track("lang-"+std::string(item.code));
            }
            ImGui::EndCombo();
        }
        ImGui::PopFont();active_language=language=="system" ? system_language : language;
        if(ui.banner)ui.art(*draw,{20*unit,available.y-67*unit},{304*unit,56*unit},{36,902},{340,958});
        ImGui::EndChild();ImGui::SetCursorPos({sidebar,0});
        ImGui::BeginChild("main",{0,0},ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar);
        const auto main_size=ImGui::GetContentRegionAvail();
        const auto main_origin=ImGui::GetCursorScreenPos();
        const float hero_height=page==2 ? 0.f : std::round(232*unit);
        if(page==2) {
            // Give the binding editor the full height; artwork stays in the sidebar.
        } else if(ui.banner) {
            auto at=ImGui::GetCursorScreenPos();
            const float source_top=page==2 ? 130.f : 75.f,source_bottom=page==2 ? 226.f : 307.f;
            ui.art(*ImGui::GetWindowDrawList(),at,{main_size.x,hero_height},{360,source_top},{1520,source_bottom});
            ImGui::Dummy({main_size.x,hero_height});
        } else {ImGui::TextWrapped("%s",tr("artwork"));ImGui::Dummy({0,hero_height-ImGui::GetTextLineHeightWithSpacing()});}
        auto* main_draw=ImGui::GetWindowDrawList();
        main_draw->AddLine({main_origin.x+.5f,main_origin.y},{main_origin.x+.5f,main_origin.y+main_size.y},IM_COL32(52,57,58,255),1);
        main_draw->AddLine({main_origin.x,main_origin.y+hero_height+.5f},{main_origin.x+main_size.x,main_origin.y+hero_height+.5f},IM_COL32(52,57,58,255),1);
        ImGui::SetCursorPos({54*unit,hero_height+32*unit});
        ImGui::BeginChild("body",{main_size.x-104*unit,main_size.y-hero_height-32*unit},ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar);
        push_font(ui.serif,36);ImGui::TextUnformatted(tr(page==1 ? "graphics_title" : sections[page]));ImGui::PopFont();
        ImGui::PushFont(ui.sans,std::round(22*unit));
        const char* hints[]={"game_hint","graphics_hint","controls_hint","audio_hint"};
        ImGui::PushStyleColor(ImGuiCol_Text,ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));ImGui::TextWrapped("%s",tr(hints[page]));ImGui::PopStyleColor();
        ImGui::Dummy({0,8*unit});ImGui::Separator();ImGui::Dummy({0,20*unit});
        auto toggle=[&](const char* id,const char* label,bool& value,bool disabled=false,const char* why=nullptr) {
            ImGui::BeginDisabled(disabled);ImGui::PushID(id);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{6*unit,6*unit});
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,3*unit);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,{12*unit,0});
            if(ImGui::CalcTextSize(label).x<=ImGui::GetContentRegionAvail().x-ImGui::GetFrameHeight()-12*unit) {ImGui::Checkbox(label,&value);track(id);}
            else {   // a label wider than the column wraps beside the box and toggles it too
                ImGui::Checkbox("##box",&value);track(id);ImGui::SameLine();
                ImGui::BeginGroup();ImGui::TextWrapped("%s",label);ImGui::EndGroup();
                if(ImGui::IsItemClicked())value=!value;
            }
            if(disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("%s",why ? why : tr("unavailable"));
            ImGui::PopStyleVar(3);ImGui::PopID();ImGui::EndDisabled();
        };
        const float footer=((page==2 ? 140.f : 212.f)+(error.empty() ? 0.f : 100.f))*unit;
        ImGui::BeginChild("settings",{0,std::max(100*unit,ImGui::GetContentRegionAvail().y-footer)},ImGuiChildFlags_None);
        if(page==0) {
            toggle("##skip-game",tr("skip"),options.skip_intro);
            ImGui::Dummy({0,8*unit});toggle("##skip-tutorials-game",tr("skip_tutorials"),options.skip_tutorials);
            ImGui::Dummy({0,25*unit});ImGui::TextWrapped("%s",tr("alt"));
        } else if(page==1) {
            const float column_gap=88*unit;
            const float column=(ImGui::GetContentRegionAvail().x-column_gap)/2;
            ImGui::BeginChild("display",{column,0},ImGuiChildFlags_None);
            ImGui::PushItemWidth(-1);ImGui::TextUnformatted(tr("window"));
            push_font(ui.sans,22);
            const char* modes[]={"windowed","borderless","exclusive"};
            const bool window_open=combo("##window",tr(modes[int(options.window_mode)]));track("window");
            if(window_open) {
                for(int i=0;i<3;++i) {if(ImGui::Selectable(tr(modes[i]),int(options.window_mode)==i))options.window_mode=uc::WindowMode(i);track("mode"+std::to_string(i));}
                ImGui::EndCombo();
            }
            ImGui::PopFont();ImGui::Dummy({0,10*unit});
            ImGui::TextUnformatted(tr("scale"));push_font(ui.sans,22);
            auto scale_label=[&](double scale) {
                const double effective=scale ? scale : uc::monitor_render_scale(monitor->w,monitor->h);
                const auto extent=uc::manual_render_extent(effective,double(monitor->w)/monitor->h);
                char value[100];std::snprintf(value,sizeof(value),"%.2g× · %u × %u",effective,extent.width,extent.height);
                return scale ? (scale==1 ? std::string("1× (Native) · ")+std::to_string(extent.width)+" × "+std::to_string(extent.height) : std::string(value)) : std::string(tr("monitor"))+" · "+std::to_string(extent.width)+" × "+std::to_string(extent.height);
            };
            const auto current_scale=scale_label(options.render_scale);
            const bool scale_open=combo("##scale",current_scale.c_str(),ImGuiComboFlags_HeightLarge);track("scale");
            if(scale_open) {
                if(ImGui::Selectable(scale_label(0).c_str(),options.render_scale==0))options.render_scale=0;
                for(const double scale:{1.,1.5,2.,3.}) {
                    if(ImGui::Selectable(scale_label(scale).c_str(),options.render_scale==scale))options.render_scale=scale;
                    if(scale==1.5)track("scale1.5");
                }
                ImGui::EndCombo();
            }
            ImGui::PopFont();ImGui::Dummy({0,10*unit});
            ImGui::TextUnformatted(tr("fps_limit"));push_font(ui.sans,22);
            const uint32_t limits[]={30,60,90,120,144,165,240,360,0};   // 0 = unlimited
            const uint32_t current_limit=options.fps_limit.value_or(options.unlock_60fps ? 60 : 30);   // no limit chosen yet: what the removed "60 FPS game update" toggle of the config said
            const auto limit_label=[&](uint32_t value) {return value ? std::to_string(value)+" FPS" : std::string(tr("unlimited"));};
            const auto current_label=limit_label(current_limit);
            const bool limit_open=combo("##fps",current_label.c_str(),ImGuiComboFlags_HeightLarge);track("fps");
            if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",tr("fps"));
            if(limit_open) {
                for(const uint32_t value:limits) {if(ImGui::Selectable(limit_label(value).c_str(),current_limit==value))options.fps_limit=value;track("fps"+std::to_string(value));}
                ImGui::EndCombo();
            }
            ImGui::PopFont();ImGui::PopItemWidth();ImGui::EndChild();
            ImGui::SameLine(0,column_gap);
            ImGui::BeginChild("enhancements",{0,0},ImGuiChildFlags_None);ImGui::PushItemWidth(-1);
            ImGui::TextUnformatted(tr("upscaling"));push_font(ui.sans,22);
            const bool checking=features_query.valid();
            const auto kind_label=[&](uc::UpscalerKind kind) {return kind==uc::UpscalerKind::dlss ? "DLSS" : kind==uc::UpscalerKind::fsr ? "FSR" : kind==uc::UpscalerKind::smaa ? "SMAA" : tr("off");};
            const bool upscaler_open=combo("##upscaling",kind_label(options.upscaler));track("upscaling");
            if(upscaler_open) {
                for(const auto kind:{uc::UpscalerKind::off,uc::UpscalerKind::smaa,uc::UpscalerKind::dlss,uc::UpscalerKind::fsr}) {
                    const bool supported=kind==uc::UpscalerKind::off || (kind==uc::UpscalerKind::smaa ? features.effects : kind==uc::UpscalerKind::dlss ? features.dlss : features.fsr);
                    const auto& reason=kind==uc::UpscalerKind::smaa ? features.effects_reason : kind==uc::UpscalerKind::dlss ? features.dlss_reason : features.fsr_reason;
                    ImGui::BeginDisabled(!supported && kind!=options.upscaler);   // a saved choice this PC cannot run stays visible and can be left, not re-picked
                    if(ImGui::Selectable(kind_label(kind),options.upscaler==kind)) {options.upscaler=kind;if(kind==uc::UpscalerKind::smaa)options.frame_generation=false;}   // SMAA has no generator
                    track(std::string("upscaler-")+std::string(uc::upscaler_name(kind)));
                    ImGui::EndDisabled();
                    if(!supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("%s",checking ? tr("checking") : reason.empty() ? tr("unsupported") : reason.c_str());
                }
                ImGui::EndCombo();
            }
            const bool upscaling=options.upscaler!=uc::UpscalerKind::off;
            const bool scaled=options.upscaler==uc::UpscalerKind::dlss || options.upscaler==uc::UpscalerKind::fsr;   // SMAA renders at the output size: no quality choice
            ImGui::PopFont();ImGui::Dummy({0,10*unit});
            ImGui::TextUnformatted(tr("quality"));push_font(ui.sans,22);
            const auto mode_label=[&](uc::UpscaleMode mode) -> std::string {
                switch(mode) {
                case uc::UpscaleMode::native: return options.upscaler==uc::UpscalerKind::fsr ? "Native AA" : "DLAA";
                case uc::UpscaleMode::quality: return tr("quality");
                case uc::UpscaleMode::balanced: return tr("balanced");
                case uc::UpscaleMode::performance: return tr("performance");
                case uc::UpscaleMode::ultra_performance: return tr("ultra_performance");
                }
                return {};
            };
            ImGui::BeginDisabled(!scaled);
            const auto current_mode=scaled ? mode_label(options.upscale_mode) : std::string("—");
            const bool mode_open=combo("##quality",current_mode.c_str());track("upscale-mode");
            if(mode_open) {
                for(const auto mode:{uc::UpscaleMode::native,uc::UpscaleMode::quality,uc::UpscaleMode::balanced,uc::UpscaleMode::performance,uc::UpscaleMode::ultra_performance}) {
                    if(ImGui::Selectable(mode_label(mode).c_str(),options.upscale_mode==mode))options.upscale_mode=mode;
                    track(std::string("upscale-")+std::string(uc::upscale_mode_name(mode)));
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::PopFont();ImGui::Dummy({0,16*unit});
            toggle("##sharpening",tr("sharpening"),options.sharpening,!upscaling);
            ImGui::BeginDisabled(!upscaling || !options.sharpening);
            float sharpness=float(options.sharpness);
            ImGui::SetNextItemWidth(-1);
            if(ImGui::SliderFloat("##sharpness",&sharpness,0.01f,1.0f,"%.2f"))options.sharpness=std::clamp(std::round(double(sharpness)*100)/100,0.01,1.0);
            track("sharpness");
            ImGui::EndDisabled();
            ImGui::Dummy({0,16*unit});
            ImGui::TextUnformatted(tr("framegen"));push_font(ui.sans,22);
            const auto generator=options.frame_generation ? options.upscaler : uc::UpscalerKind::off;
            const bool generator_open=combo("##framegen",kind_label(generator));track("framegen");
            if(generator_open) {
                for(const auto kind:{uc::UpscalerKind::off,uc::UpscalerKind::dlss,uc::UpscalerKind::fsr}) {
                    const bool supported=kind==uc::UpscalerKind::off || (kind==uc::UpscalerKind::dlss ? features.dlss_g : features.fsr_fg);
                    const auto& reason=kind==uc::UpscalerKind::dlss ? features.dlss_g_reason : features.fsr_fg_reason;
                    ImGui::BeginDisabled(!supported);
                    if(ImGui::Selectable(kind_label(kind),generator==kind)) {
                        options.frame_generation=kind!=uc::UpscalerKind::off;
                        if(options.frame_generation)options.upscaler=kind;
                    }
                    track(std::string("framegen-")+std::string(uc::upscaler_name(kind)));
                    ImGui::EndDisabled();
                    if(!supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("%s",checking ? tr("checking") : reason.empty() ? tr("unsupported") : reason.c_str());
                }
                ImGui::EndCombo();
            }
            ImGui::PopFont();
            if(options.frame_generation && options.upscaler!=uc::UpscalerKind::off) {
                const uint32_t most=options.upscaler==uc::UpscalerKind::dlss ? std::clamp<uint32_t>(features.dlss_g_max_multiplier,2,6) : 2;   // FSR FG: one generated frame
                if(features_ready)options.frame_generation_multiplier=std::clamp<uint32_t>(options.frame_generation_multiplier,2,most);   // the saved choice survives until the report says what the PC allows
                ImGui::TextUnformatted(tr("multiplier"));push_font(ui.sans,22);
                const auto current_multiplier=std::to_string(options.frame_generation_multiplier)+"×";
                const bool multiplier_open=combo("##multiplier",current_multiplier.c_str());track("multiplier");
                if(multiplier_open) {
                    for(uint32_t value=2;value<=most;++value) {if(ImGui::Selectable((std::to_string(value)+"×").c_str(),options.frame_generation_multiplier==value))options.frame_generation_multiplier=value;track("multiplier"+std::to_string(value));}
                    ImGui::EndCombo();
                }
                ImGui::PopFont();
                if(options.fps_limit.value_or(options.unlock_60fps ? 60 : 30)) {
                    ImGui::PushStyleColor(ImGuiCol_Text,ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::TextWrapped("%s: %u FPS",tr("output_fps"),options.fps_limit.value_or(options.unlock_60fps ? 60 : 30)*options.frame_generation_multiplier);
                    ImGui::PopStyleColor();
                }
            }
            ImGui::Dummy({0,8*unit});ImGui::SeparatorText(tr("effects"));
            ImGui::TextUnformatted(tr("anisotropy"));push_font(ui.sans,22);
            {
                const uint32_t most=features_ready ? features.anisotropy_max : 16;   // until the report arrives the renderer's own clamp decides
                const auto level_label=[&](uint32_t level) {return level==1 ? std::string(tr("off")) : std::to_string(level)+"×";};
                const bool anisotropy_open=combo("##anisotropy",level_label(options.effects.anisotropy).c_str());track("anisotropy");
                if(anisotropy_open) {
                    for(const uint32_t level:{1u,2u,4u,8u,16u}) {
                        const bool supported=level<=most;
                        ImGui::BeginDisabled(!supported && level!=options.effects.anisotropy);   // a saved level this GPU cannot run stays visible and can be left
                        if(ImGui::Selectable(level_label(level).c_str(),options.effects.anisotropy==level))options.effects.anisotropy=level;
                        track("anisotropy-"+std::to_string(level));
                        ImGui::EndDisabled();
                        if(!supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("%s",tr("unsupported"));
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::PopFont();
            ImGui::Dummy({0,8*unit});
            toggle("##ppl",tr("per_pixel_lighting"),options.effects.per_pixel_lighting,features_ready && !features.effects && !options.effects.per_pixel_lighting,tr("effects_unavailable"));
            ImGui::Dummy({0,8*unit});
            bool no_bloom=!options.bloom;toggle("##bloom",tr("bloom"),no_bloom);options.bloom=!no_bloom;   // checked = no glow; graphics.bloom keeps true = glow
            ImGui::Dummy({0,8*unit});toggle("##skip",tr("skip"),options.skip_intro);
            ImGui::Dummy({0,8*unit});toggle("##skip-tutorials",tr("skip_tutorials"),options.skip_tutorials);
            ImGui::PopItemWidth();ImGui::EndChild();
        } else if(page==2) {
            push_font(ui.sans,22);
            ImGui::TextUnformatted(tr("style"));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x*.57f);
            const char* styles[]={tr("gray"),tr("cyan")};
            const bool style_combo=combo("##style",styles[size_t(options.prompt_style)]);track("style");
            if(style_combo) {
                for(size_t i=0;i<2;++i) {if(ImGui::Selectable(styles[i],size_t(options.prompt_style)==i))options.prompt_style=uc::PromptStyle(i);track(i ? "style3" : "style2");}
                ImGui::EndCombo();
            }
            ImGui::SameLine(0,25*unit);
            const float ph=44*unit;const auto p=ImGui::GetCursorScreenPos();auto* preview=ImGui::GetWindowDrawList();
            for(int i=0;i<3;++i)uc::draw_mouse_icon(*preview,{p.x+i*ph,p.y},{ph*.65f,ph},{i==2 ? uc::PhysicalKind::wheel : uc::PhysicalKind::mouse,uint16_t(i==0 ? 1 : i==1 ? 3 : 1)},false,options.prompt_style);
            ImGui::Dummy({ph*3,ph});ImGui::Spacing();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{12*unit,10*unit});
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8*unit,8*unit});
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,{4*unit,3*unit});
            if(button(tr("defaults"),"defaults"))editor.restore_defaults();
            if(editor.capturing())ImGui::TextWrapped("%s",tr("capture"));
            if(editor.error()) {ImGui::TextWrapped("%s",tr("rejected"));if(ImGui::TreeNode(tr("details"))) {ImGui::TextWrapped("%s",editor.error()->c_str());ImGui::TreePop();}}
            ImGui::BeginDisabled(editor.capturing().has_value());
            if(ImGui::BeginTable("bindings",4,ImGuiTableFlags_PadOuterX|ImGuiTableFlags_ScrollY|ImGuiTableFlags_SizingStretchProp,{0,std::max(100*unit,ImGui::GetContentRegionAvail().y)})) {
                ImGui::TableSetupColumn(tr("action"),ImGuiTableColumnFlags_WidthStretch,.40f);
                ImGui::TableSetupColumn(tr("bindings"),ImGuiTableColumnFlags_WidthStretch,.60f);
                ImGui::TableSetupColumn("+",ImGuiTableColumnFlags_WidthFixed,46*unit);
                ImGui::TableSetupColumn(tr("clear"),ImGuiTableColumnFlags_WidthFixed,ImGui::CalcTextSize(tr("clear")).x+28*unit);
                for(size_t i=0;i<size_t(uc::Action::count);++i) {
                    auto action=uc::Action(i);ImGui::PushID(int(i));ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::AlignTextToFramePadding();ImGui::TextWrapped("%s",tr(uc::action_names[i]));
                    ImGui::TableSetColumnIndex(1);const auto& entries=editor.bindings()[i];
                    std::string names;
                    for(const auto& entry:entries) {if(!names.empty())names+=", ";names+=binding_name(entry);}
                    if(names.empty())names=tr("unbound");
                    if(button(names.c_str(),"replace"+std::to_string(i),{-1,0}))editor.begin_capture(action,true);
                    if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",names.c_str());
                    if(ImGui::BeginPopupContextItem("binding-menu")) {
                    for(size_t j=0;j<entries.size();++j) {
                        ImGui::PushID(int(j));const auto label=std::string(tr("remove"))+": "+binding_name(entries[j]);
                        const bool remove=ImGui::MenuItem(label.c_str());ImGui::PopID();
                        if(remove) {editor.remove(action,j);break;}
                    }
                    ImGui::EndPopup();
                    }
                    ImGui::TableSetColumnIndex(2);
                    const bool special=action==uc::Action::grenade || action==uc::Action::knife || action==uc::Action::qte_shake;
                    if(button("+","add"+std::to_string(i),{-1,0})) {
                        if(special)ImGui::OpenPopup("add-binding");else editor.begin_capture(action);
                    }
                    if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",tr("add"));
                    if(ImGui::BeginPopup("add-binding")) {
                        if(ImGui::MenuItem(tr("add")))editor.begin_capture(action);
                        if(action==uc::Action::grenade && ImGui::MenuItem(tr("chord")))editor.add(action,{uc::BindingKind::interact_fire,{},{}});
                        if(action==uc::Action::knife && ImGui::MenuItem(tr("gesture")))editor.add(action,{uc::BindingKind::interact_motion,{},{}});
                        if(action==uc::Action::qte_shake && ImGui::MenuItem(tr("motion")))editor.add(action,{uc::BindingKind::qte_motion,{},{}});
                        ImGui::EndPopup();
                    }
                    ImGui::TableSetColumnIndex(3);
                    if(button(tr("clear"),"clear"+std::to_string(i),{-1,0}))editor.clear(action);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndDisabled();ImGui::PopStyleVar(3);ImGui::PopFont();
        } else ImGui::TextWrapped("%s",tr("audio_info"));
        ImGui::EndChild();ImGui::Separator();ImGui::Dummy({0,10*unit});
        if(!error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text,{1,.52f,.52f,1});ImGui::TextWrapped("%s",tr(error.find("changed since loading")!=std::string::npos ? "stale" : "error"));ImGui::PopStyleColor();
            if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",error.c_str());
            if(button(tr("reload_config"),"reload")) {reload();game_path=utf8(options.game);}
        }
        const auto foot=ImGui::GetCursorScreenPos();const float width=ImGui::GetContentRegionAvail().x;
        const float path_width=width*.55f,launch_x=foot.x+width*.605f;
        std::error_code path_error;const bool game_found=fs::is_regular_file(options.game/L"DATA"/L"sys"/L"main.dol",path_error);
        if(page!=2) {
        ImGui::BeginGroup();ImGui::TextUnformatted(tr("folder"));
        ImGui::PushFont(ui.sans,std::round(22*unit));ImGui::SetNextItemWidth(path_width-70*unit);
        if(ImGui::InputText("##game",&game_path))options.game=fs::path(std::u8string(game_path.begin(),game_path.end()));
        ImGui::SameLine(0,12*unit);
        if(button("##browse","browse",{58*unit,ImGui::GetFrameHeight()})) {
            try {auto selected=options.game;if(uc::launcher::choose_game_folder(ui.window,selected)) {options.game=selected;game_path=utf8(selected);}}
            catch(const std::exception& e) {error=e.what();}
        }
        auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();auto* folder_draw=ImGui::GetWindowDrawList();
        const ImVec2 c{std::round((a.x+b.x)/2),std::round((a.y+b.y)/2)};
        ImVec2 folder_points[]={{c.x-13*unit,c.y-9*unit},{c.x-3*unit,c.y-9*unit},{c.x+1*unit,c.y-5*unit},{c.x+13*unit,c.y-5*unit},{c.x+13*unit,c.y+9*unit},{c.x-13*unit,c.y+9*unit}};
        folder_draw->AddConvexPolyFilled(folder_points,6,IM_COL32(192,196,196,255));
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",tr("browse"));ImGui::PopFont();
        ImGui::Dummy({0,20*unit});
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+path_width);ImGui::PushStyleColor(ImGuiCol_Text,game_found ? ImVec4{.47f,.74f,.52f,1} : ImVec4{.85f,.68f,.42f,1});
        if(game_found) {
            const auto p=ImGui::GetCursorScreenPos();auto* d=ImGui::GetWindowDrawList();
            d->AddCircleFilled({p.x+10*unit,p.y+13*unit},9*unit,IM_COL32(77,145,89,255));
            d->AddLine({p.x+6*unit,p.y+13*unit},{p.x+9*unit,p.y+16*unit},IM_COL32(14,45,25,255),2*unit);
            d->AddLine({p.x+9*unit,p.y+16*unit},{p.x+15*unit,p.y+9*unit},IM_COL32(14,45,25,255),2*unit);
            ImGui::Dummy({22*unit,0});ImGui::SameLine(0,9*unit);
        }
        ImGui::TextWrapped("%s",tr(game_found ? "found" : "missing"));ImGui::PopStyleColor();ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        }
        const float button_y=foot.y+(page==2 ? 0.f : 32*unit);
        ImGui::SetCursorScreenPos({launch_x,button_y});
        ImGui::BeginDisabled(blocked || editor.capturing().has_value());
        const bool running=uc::game_running(process);ImGui::BeginDisabled(running || launch_requested || !game_found);
        ImGui::PushStyleColor(ImGuiCol_Button,{.43f,.045f,.07f,1});ImGui::PushStyleColor(ImGuiCol_ButtonHovered,{.62f,.075f,.12f,1});ImGui::PushStyleColor(ImGuiCol_Border,{.67f,.10f,.14f,1});
        push_font(ui.serif,26);
        if(button(tr(running ? "running" : launch_requested ? "preparing" : "play"),"launch",{width*.395f,74*unit}) && save(options)) {
            launch_requested=true;
            std::fprintf(stderr,"[cpu-calibration] Play requested: %s\n",cpu_ready ? "ready" : "waiting");
        }
        ImGui::PopFont();ImGui::PopStyleColor(3);ImGui::EndDisabled();
        ImGui::EndDisabled();ImGui::PopFont();ImGui::EndChild();ImGui::EndChild();ImGui::PopFont();
        // Every change is saved at once. An unfinished game folder is not saved (the last good one stays); a refused save is not retried until something changes.
        if(!blocked && !editor.capturing()) {
            auto chosen=options;if(!game_found)chosen.game=saved_options.game;
            const bool changed=editor.bindings()!=document.input || !same_options(chosen,saved_options) || language!=saved_language;
            const bool same_as_failed=failed && editor.bindings()==failed_bindings && same_options(chosen,failed_options) && language==failed_language;
            if(changed && !same_as_failed && !save(chosen)) {failed=true;failed_options=chosen;failed_bindings=editor.bindings();failed_language=language;}
        }
        const bool check_capture_wait=!smoke_exe.empty() && !smoke_capture_wait_checked && launch_requested && cpu_ready && !editor.capturing();
        if(check_capture_wait)editor.begin_capture(uc::Action::fire,false);
        if(launch_requested && cpu_ready && !done && !blocked && !editor.capturing() && game_found) {
            launch_requested=false;
            if(save(options)) {
                try {process=uc::launch_game(smoke_exe.empty() ? directory/L"uc.exe" : smoke_exe,config_path,smoke_exe.empty() ? nullptr : &diagnostic,cpu_choice.cpu);}
                catch(const std::exception& e) {error=e.what();}
                if(process && smoke.empty() && smoke_exe.empty())done=true;   // the launcher closes once the game has started; smoke runs keep it to watch the game
            }
        }
        if(check_capture_wait) {
            if(!launch_requested || process)throw std::runtime_error("pending Play lost during binding capture");
            editor.cancel_capture();smoke_capture_wait_checked=true;
        }
        ImGui::End();ImGui::Render();
        if(!SDL_SetRenderDrawColor(ui.renderer,18,19,22,255) || !SDL_RenderClear(ui.renderer))throw std::runtime_error(SDL_GetError());
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),ui.renderer);
        if(!smoke.empty()) {
            if(frame==24) {if(!editor.bindings()[size_t(uc::Action::fire)].empty())throw std::runtime_error("Clear All UI failed");ui.capture(smoke/L"cleared.bmp");}
            if(frame==14)ui.capture(smoke/L"tab.bmp");
            if(frame==42) {if(uc::load_config(config_path).input[size_t(uc::Action::fire)]!=std::vector<uc::Binding>{{uc::BindingKind::physical,{uc::PhysicalKind::key,41},{}}})throw std::runtime_error("Esc capture/save UI failed");ui.capture(smoke/L"saved.bmp");}
            if(frame==52) {if(error.find("changed since loading")==std::string::npos)throw std::runtime_error("stale-save error UI failed");ui.capture(smoke/L"error.bmp");}
            if(frame==66) {if(editor.bindings()!=uc::default_bindings())throw std::runtime_error("restore defaults UI failed");ui.capture(smoke/L"defaults.bmp");}
            if(frame==96 || frame==110) {
                const auto expected=frame==96 ? uc::PromptStyle::sculpted : uc::PromptStyle::monochrome;
                ui.capture(smoke/(frame==96 ? L"style3.bmp" : L"style2.bmp"));
                if(options.prompt_style!=expected || uc::options_from_config(uc::load_config(config_path)).prompt_style!=expected || !same_options(options,saved_options))throw std::runtime_error("style select/save UI failed: selected="+std::to_string(int(options.prompt_style))+" expected="+std::to_string(int(expected))+" dirty="+std::to_string(!same_options(options,saved_options))+" error="+error);
            }
            for(size_t i=0;i<uc::launcher::languages().size();++i)if(frame==126+i*18) {
                const auto expected=uc::launcher::languages()[i].code;
                if(language!=expected || active_language!=expected || uc::launcher::config_language(uc::load_config(config_path))!=expected || language!=saved_language)throw std::runtime_error("language select/save UI failed: "+std::string(expected));
                ui.capture(smoke/(std::string(expected)+".bmp"));
                reload();if(active_language!=expected)throw std::runtime_error("language reload failed");
            }
            if(frame==370 || frame==378 || frame==386 || frame==394)ui.capture(smoke/("small-"+std::to_string(frame)+".bmp"));
            if(frame==116 || frame==134 || frame==402 || frame==412)ui.capture(smoke/("popup-"+std::to_string(frame)+".bmp"));
            if(frame==422) {
                const auto saved=uc::options_from_config(uc::load_config(config_path));
                if(saved.render_scale!=1.5 || saved.window_mode!=uc::WindowMode::borderless)throw std::runtime_error("window/scale popup select/save failed");
            }
            if(frame==428) {
                if(uc::options_from_config(uc::load_config(config_path)).fps_limit!=144u || options.fps_limit!=144u)throw std::runtime_error("frame rate limit select/save UI failed");
            }
            if(frame==432) {
                const auto saved=uc::options_from_config(uc::load_config(config_path));
                if(saved.upscaler!=uc::UpscalerKind::dlss || options.upscaler!=uc::UpscalerKind::dlss || !saved.frame_generation || !options.frame_generation)throw std::runtime_error("DLSS generation select/save from Off failed");
                ui.capture(smoke/L"upscaling.bmp");
            }
            if(frame==440 || frame==456) {
                const auto saved=uc::options_from_config(uc::load_config(config_path));
                if(saved.frame_generation || options.frame_generation)throw std::runtime_error("generation Off select/save failed");
            }
            if(frame==448) {
                const auto saved=uc::options_from_config(uc::load_config(config_path));
                if(saved.upscaler!=uc::UpscalerKind::fsr || !saved.frame_generation || saved.frame_generation_multiplier!=2)throw std::runtime_error("FSR generation select/save failed");
                ui.capture(smoke/L"framegen-fsr.bmp");
            }
            if(frame==488) {
                if(uc::load_config(config_path).input[size_t(uc::Action::weapon_previous)]!=std::vector<uc::Binding>{{uc::BindingKind::physical,{uc::PhysicalKind::key,26},{}}})throw std::runtime_error("grid replacement/save failed");
                ui.capture(smoke/L"grid-replaced.bmp");
            }
            if(frame>=492) {
                if(smoke_exe.empty())done=true;
                else {
                    if(!process && !launch_requested)throw std::runtime_error("launch button failed: "+error);
                    if(!process) {
                        if(SDL_GetTicks()-start>35000)throw std::runtime_error("calibration wait timed out");
                    } else if(!uc::game_running(process)) {
                        DWORD code=0;if(!GetExitCodeProcess(process.get(),&code) || code!=0)throw std::runtime_error("game launch smoke failed");
                        done=true;
                    } else if(SDL_GetTicks()-start>35000)throw std::runtime_error("game launch smoke timed out");
                }
            }
        }
        if(!SDL_RenderPresent(ui.renderer))throw std::runtime_error(SDL_GetError());
        ++frame;
    }
    if(!smoke_exe.empty() && !smoke_capture_wait_checked)throw std::runtime_error("pending Play capture check did not run");
    if(!smoke.empty())std::fprintf(stderr,"launcher smoke passed: clear/capture/save/stale/defaults/style2/style3/13 languages/glyph coverage; no OS input\n");
    return 0;
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    int count=0;auto arguments=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!arguments)return 2;
    int result=0;try {result=run(count,arguments);}catch(const std::exception& e) {std::fprintf(stderr,"launcher failed: %s\n",e.what());result=2;}
    LocalFree(arguments);return result;
}
