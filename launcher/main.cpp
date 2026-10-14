#include "binding_editor.h"
#include "launch.h"
#include "options_config.h"
#include "render_scale.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
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
    std::vector<char> font;
    ~Ui() {
        if(backend)ImGui_ImplSDLRenderer3_Shutdown();
        if(platform)ImGui_ImplSDL3_Shutdown();
        if(context)ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
    }
    void init(bool diagnostic) {
        if(!SDL_Init(SDL_INIT_VIDEO))throw std::runtime_error(SDL_GetError());
        window=SDL_CreateWindow("Umbrella Chronicles — настройки",1000,780,SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIDDEN|SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if(!window)throw std::runtime_error(SDL_GetError());
        renderer=SDL_CreateRenderer(window,nullptr);
        if(!renderer || !SDL_SetRenderVSync(renderer,1))throw std::runtime_error(SDL_GetError());
        ImGui::CreateContext();context=true;
        auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
        if(diagnostic)io.ConfigInputTrickleEventQueue=false;
        ImGui::StyleColorsDark();
        const float dpi=SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        if(dpi>0) {ImGui::GetStyle().ScaleAllSizes(dpi);ImGui::GetStyle().FontScaleDpi=dpi;}
        wchar_t windows[32768];const auto length=GetWindowsDirectoryW(windows,32768);
        if(!length || length>=32768)throw std::runtime_error("Windows font directory unavailable");
        std::ifstream source(fs::path(windows)/L"Fonts"/L"segoeui.ttf",std::ios::binary);
        font.assign(std::istreambuf_iterator<char>(source),{});
        if(font.empty() || font.size()>16*1024*1024)throw std::runtime_error("Segoe UI font unavailable");
        ImFontConfig config;config.FontDataOwnedByAtlas=false;
        if(!io.Fonts->AddFontFromMemoryTTF(font.data(),int(font.size()),18,&config))throw std::runtime_error("cannot load launcher font");
        platform=ImGui_ImplSDL3_InitForSDLRenderer(window,renderer);
        backend=ImGui_ImplSDLRenderer3_Init(renderer);
        if(!platform || !backend)throw std::runtime_error("ImGui backend initialization failed");
        if(diagnostic) {
            auto hwnd=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
            if(!hwnd)throw std::runtime_error("launcher window handle unavailable");
            ShowWindow(hwnd,SW_SHOWNOACTIVATE);
        } else if(!SDL_ShowWindow(window))throw std::runtime_error(SDL_GetError());
    }
    void capture(const fs::path& path) {
        auto* surface=SDL_RenderReadPixels(renderer,nullptr);
        if(!surface)throw std::runtime_error(SDL_GetError());
        const bool saved=SDL_SaveBMP(surface,utf8(path).c_str());SDL_DestroySurface(surface);
        if(!saved)throw std::runtime_error(SDL_GetError());
    }
};
constexpr std::array<const char*,size_t(uc::Action::count)> labels={"Взаимодействие","Выстрел","Предыдущее оружие","Следующее оружие","Перезарядка","Граната","Нож","Пауза","Подтверждение","Назад","Освободить курсор","Скриншот","QTE: встряхивание"};
std::string physical_name(uc::PhysicalInput input) {
    if(input.kind==uc::PhysicalKind::key) {
        const auto* name=SDL_GetKeyName(SDL_GetKeyFromScancode(SDL_Scancode(input.code),SDL_KMOD_NONE,false));
        return name && *name ? name : "Клавиша "+std::to_string(input.code);
    }
    if(input.kind==uc::PhysicalKind::wheel)return input.code==1 ? "Колесо вверх" : "Колесо вниз";
    if(input.code==1)return "ЛКМ";if(input.code==3)return "ПКМ";if(input.code==2)return "СКМ";
    return "Мышь "+std::to_string(input.code);
}
std::string binding_name(const uc::Binding& binding) {
    if(binding.kind==uc::BindingKind::interact_fire)return "Удержание взаимодействия + выстрел";
    if(binding.kind==uc::BindingKind::interact_motion)return "Движение мыши + взаимодействие";
    if(binding.kind==uc::BindingKind::qte_motion)return "Резкие движения мышью";
    std::string text;for(auto modifier:binding.modifiers)text+=physical_name(modifier)+" + ";
    return text+physical_name(binding.input);
}
bool same_options(const uc::Options& a,const uc::Options& b) {
    return a.game==b.game && a.render_scale==b.render_scale && a.window_mode==b.window_mode && a.width==b.width && a.height==b.height && a.skip_intro==b.skip_intro && a.unlock_60fps==b.unlock_60fps;
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
    uc::Options diagnostic;
    if(!smoke_exe.empty()) {
        diagnostic.test_control=fs::absolute(smoke/L"control.txt");diagnostic.save_root=fs::absolute(smoke/L"saves");
        fs::copy(L"S:/Games/Umbrella Chronicles/saves",diagnostic.save_root,fs::copy_options::recursive|fs::copy_options::skip_existing);
    }
    uc::ConfigDocument document;uc::Options options,saved_options;uc::BindingEditor editor;uc::GameProcess process;
    std::string error;bool blocked=false;
    auto reload=[&] {
        try {auto loaded=uc::load_config(config_path);auto settings=uc::options_from_config(loaded);
            editor=uc::BindingEditor(loaded.input);document=std::move(loaded);options=saved_options=settings;blocked=false;error.clear();
        } catch(const std::exception& e) {error=e.what();blocked=true;}
    };
    reload();std::string game_path=utf8(options.game);
    Ui ui;ui.init(!smoke.empty());
    const auto* monitor=SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if(!monitor || monitor->w<=0 || monitor->h<=0)throw std::runtime_error("monitor mode unavailable");
    std::map<std::string,ImVec2> positions;
    auto track=[&](const std::string& name) {if(!smoke.empty()) {auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();positions[name]={(a.x+b.x)/2,(a.y+b.y)/2};}};
    auto button=[&](const char* label,const std::string& id) {bool clicked=ImGui::Button(label);track(id);return clicked;};
    auto save=[&] {
        try {auto next=document;next.input=editor.bindings();uc::store_options(next,options);uc::save_config(config_path,next);
            document=std::move(next);saved_options=options;error.clear();return true;
        } catch(const std::exception& e) {error=e.what();return false;}
    };
    bool done=false;unsigned frame=0;const auto start=SDL_GetTicks();
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
        ImGui_ImplSDLRenderer3_NewFrame();ImGui_ImplSDL3_NewFrame();
        if(!smoke.empty()) {
            auto& io=ImGui::GetIO();io.AddFocusEvent(true);
            std::map<unsigned,std::string> clicks={{10,"controls"},{18,"clear1"},{26,"add1"},{38,"save"},{48,"save"}};
            if(smoke_exe.empty())clicks[58]="defaults";
            else {clicks[56]="reload";clicks[62]="defaults";clicks[66]="save";clicks[70]="launch";}
            for(auto [when,id]:clicks)if(frame>=when-2 && frame<=when+1) {
                if(!positions.contains(id))throw std::runtime_error("launcher smoke widget missing: "+id);
                const auto point=positions.at(id);io.AddMousePosEvent(point.x,point.y);
                if(frame==when || frame==when+1)io.AddMouseButtonEvent(0,frame==when);
            }
            if(frame==32) {if(!editor.capturing())throw std::runtime_error("capture UI did not start");editor.event({uc::PhysicalKind::key,9},true);}
            if(frame==44) {auto other=uc::load_config(config_path);other.properties["smoke_external_edit"]=true;uc::save_config(config_path,other);}
        }
        ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Настройки",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("Resident Evil: The Umbrella Chronicles");
        if(ImGui::BeginTabBar("settings")) {
            if(ImGui::BeginTabItem("Основные")) {
                if(ImGui::InputText("Папка игры",&game_path))options.game=fs::path(std::u8string(game_path.begin(),game_path.end()));
                int mode=int(options.window_mode);
                if(ImGui::Combo("Режим окна",&mode,"Windowed\0Borderless Window\0Exclusive Fullscreen\0"))options.window_mode=uc::WindowMode(mode);
                bool native_monitor=options.render_scale==0;
                if(ImGui::Checkbox("Разрешение монитора при запуске",&native_monitor))options.render_scale=native_monitor ? 0 : uc::monitor_render_scale(monitor->w,monitor->h);
                ImGui::BeginDisabled(native_monitor);
                float scale=float(options.render_scale ? options.render_scale : uc::monitor_render_scale(monitor->w,monitor->h));
                if(ImGui::SliderFloat("Множитель Native720p",&scale,.5f,6.f,"%.2fx"))options.render_scale=scale;
                ImGui::EndDisabled();
                try {auto extent=uc::manual_render_extent(scale,double(monitor->w)/monitor->h);ImGui::Text("%u × %u → окно / экран",extent.width,extent.height);}catch(const std::invalid_argument& e) {ImGui::TextWrapped("%s",e.what());}
                ImGui::Checkbox("Пропустить вступительные ролики",&options.skip_intro);
                ImGui::Checkbox("60 FPS игрового обновления (экспериментально)",&options.unlock_60fps);
                ImGui::TextUnformatted("Лимит вывода:240 FPS. ALT освобождает курсор в Windowed.");
                ImGui::EndTabItem();
            }
            const bool controls=ImGui::BeginTabItem("Управление");track("controls");
            if(controls) {
                if(button("Восстановить стандартные назначения","defaults"))editor.restore_defaults();
                if(editor.capturing())ImGui::TextWrapped("Нажмите кнопку или поверните колесо. Esc — отменить. Модификатор отдельно: нажмите и отпустите.");
                if(editor.error())ImGui::TextWrapped("Назначение не добавлено: %s",editor.error()->c_str());
                ImGui::BeginDisabled(editor.capturing().has_value());
                if(ImGui::BeginTable("bindings",3,ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_ScrollY|ImGuiTableFlags_SizingStretchProp,{0,-160})) {
                    ImGui::TableSetupColumn("Действие",ImGuiTableColumnFlags_WidthFixed,200);
                    ImGui::TableSetupColumn("Назначения");ImGui::TableSetupColumn("Изменить",ImGuiTableColumnFlags_WidthFixed,240);ImGui::TableHeadersRow();
                    for(size_t i=0;i<size_t(uc::Action::count);++i) {
                        auto action=uc::Action(i);ImGui::PushID(int(i));ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::TextUnformatted(labels[i]);
                        ImGui::TableSetColumnIndex(1);const auto& entries=editor.bindings()[i];
                        if(entries.empty())ImGui::TextDisabled("Не назначено");
                        for(size_t j=0;j<entries.size();++j) {
                            ImGui::PushID(int(j));ImGui::TextWrapped("%s",binding_name(entries[j]).c_str());ImGui::SameLine();
                            if(ImGui::SmallButton("Удалить")) {editor.remove(action,j);ImGui::PopID();break;}
                            for(size_t other=0;other<size_t(uc::Action::count);++other)if(other!=i)
                                for(const auto& shared:editor.bindings()[other])if(shared.kind==entries[j].kind && shared.input==entries[j].input && shared.modifiers.size()==entries[j].modifiers.size() && std::is_permutation(shared.modifiers.begin(),shared.modifiers.end(),entries[j].modifiers.begin()))
                                    ImGui::TextDisabled("Также: %s",labels[other]);
                            ImGui::PopID();
                        }
                        ImGui::TableSetColumnIndex(2);
                        if(button("Добавить","add"+std::to_string(i)))editor.begin_capture(action);
                        ImGui::SameLine();if(button("Очистить все","clear"+std::to_string(i)))editor.clear(action);
                        if(ImGui::SmallButton("Назначить Esc"))editor.add(action,{uc::BindingKind::physical,{uc::PhysicalKind::key,41},{}});
                        if(action==uc::Action::grenade && ImGui::SmallButton("Добавить сочетание"))editor.add(action,{uc::BindingKind::interact_fire,{},{}});
                        if(action==uc::Action::knife && ImGui::SmallButton("Добавить жест"))editor.add(action,{uc::BindingKind::interact_motion,{},{}});
                        if(action==uc::Action::qte_shake && ImGui::SmallButton("Добавить движения мышью"))editor.add(action,{uc::BindingKind::qte_motion,{},{}});
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndDisabled();ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if(!error.empty())ImGui::TextWrapped("Ошибка: %s",error.c_str());
        const bool dirty=editor.bindings()!=document.input || !same_options(options,saved_options);
        if(dirty)ImGui::TextUnformatted("Есть несохранённые изменения");
        if((blocked || !error.empty()) && button("Загрузить config заново (сбросить правки)","reload")) {reload();game_path=utf8(options.game);}
        ImGui::BeginDisabled(blocked || editor.capturing().has_value());
        if(button("Сохранить","save"))save();ImGui::SameLine();
        const bool running=uc::game_running(process);
        ImGui::BeginDisabled(running);
        if(button("Запустить игру","launch") && save()) {try {process=uc::launch_game(smoke_exe.empty() ? directory/L"uc.exe" : smoke_exe,config_path,smoke_exe.empty() ? nullptr : &diagnostic);}catch(const std::exception& e) {error=e.what();}}
        ImGui::EndDisabled();ImGui::EndDisabled();if(running) {ImGui::SameLine();ImGui::TextUnformatted("Игра запущена");}
        ImGui::End();ImGui::Render();
        if(!SDL_SetRenderDrawColor(ui.renderer,18,19,22,255) || !SDL_RenderClear(ui.renderer))throw std::runtime_error(SDL_GetError());
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),ui.renderer);
        if(!smoke.empty()) {
            if(frame==24) {if(!editor.bindings()[size_t(uc::Action::fire)].empty())throw std::runtime_error("Clear All UI failed");ui.capture(smoke/L"cleared.bmp");}
            if(frame==14)ui.capture(smoke/L"tab.bmp");
            if(frame==42) {if(uc::load_config(config_path).input[size_t(uc::Action::fire)]!=std::vector<uc::Binding>{{uc::BindingKind::physical,{uc::PhysicalKind::key,9},{}}})throw std::runtime_error("capture/save UI failed");ui.capture(smoke/L"saved.bmp");}
            if(frame==52) {if(error.find("changed since loading")==std::string::npos)throw std::runtime_error("stale-save error UI failed");ui.capture(smoke/L"error.bmp");}
            if(frame==66) {if(editor.bindings()!=uc::default_bindings())throw std::runtime_error("restore defaults UI failed");ui.capture(smoke/L"defaults.bmp");}
            if(frame>=76) {
                if(smoke_exe.empty())done=true;
                else {
                    if(!process)throw std::runtime_error("launch button failed: "+error);
                    if(!uc::game_running(process)) {
                        DWORD code=0;if(!GetExitCodeProcess(process.get(),&code) || code!=0)throw std::runtime_error("game launch smoke failed");
                        done=true;
                    } else if(SDL_GetTicks()-start>35000)throw std::runtime_error("game launch smoke timed out");
                }
            }
        }
        if(!SDL_RenderPresent(ui.renderer))throw std::runtime_error(SDL_GetError());
        ++frame;
    }
    if(!smoke.empty())std::fprintf(stderr,"launcher smoke passed: clear/capture/save/stale/defaults; no OS input\n");
    return 0;
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    int count=0;auto arguments=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!arguments)return 2;
    int result=0;try {result=run(count,arguments);}catch(const std::exception& e) {std::fprintf(stderr,"launcher failed: %s\n",e.what());result=2;}
    LocalFree(arguments);return result;
}
