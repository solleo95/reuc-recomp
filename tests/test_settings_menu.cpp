#include "check.h"
#include "settings_menu.h"
#include "vulkan_renderer.h"
#include <intrin.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <cstdio>
#include <chrono>
#include <cmath>

static void exercise(uc::SettingsMenu& menu,uc::LiveSettings& live,const std::function<void()>& frame,const std::function<void(const char*)>& capture) {
    for(int i=0;i<4;++i) frame();
    const auto spacing=ImGui::GetStyle().ItemInnerSpacing.x;
    for(int i=0;i<120;++i) frame();
    CHECK(ImGui::GetStyle().ItemInnerSpacing.x==spacing);
    auto* window=ImGui::FindWindowByName("##settings"); CHECK(window);
    CHECK(std::abs(window->Size.x-746)<2 && std::abs(window->Size.y-1200)<2);
    auto& context=*ImGui::GetCurrentContext();
    CHECK(context.TabBars.GetAliveCount()==1);
    auto* tabs=context.TabBars.GetByIndex(0);
    std::printf("tabs %d, menu %.0fx%.0f\n",tabs->Tabs.Size,window->Size.x,window->Size.y);
    CHECK(tabs->Tabs.Size==3);
    CHECK(tabs->Tabs[2].Offset+tabs->Tabs[2].Width<=tabs->BarRect.GetWidth());
    // Open the first combo through ImGui input, then inspect the actual popup geometry.
    ImGuiWindow* graphics=nullptr;
    for(auto* candidate:context.Windows) if(candidate->ParentWindow==window && (candidate->Flags&ImGuiWindowFlags_ChildWindow)) graphics=candidate;
    CHECK(graphics);
    CHECK(graphics->FontRefSize==29);
    auto& io=ImGui::GetIO();
    const float unit=io.DisplaySize.y/1080.f*.75f, text=std::round(22*io.DisplaySize.y/1080.f);
    const auto& style=ImGui::GetStyle();
    const float height=text+2*style.FramePadding.y;
    const float first=style.ItemSpacing.y+text;
    const auto click=[&](ImVec2 position) {
        io.AddMousePosEvent(position.x,position.y); frame();
        io.AddMouseButtonEvent(0,true); frame();
        io.AddMouseButtonEvent(0,false); frame(); frame();
    };
    capture("main");
    click({graphics->Pos.x+40,graphics->Pos.y+first+height*.5f});
    auto* popup=ImGui::FindWindowByName("##Combo_00"); CHECK(popup && popup->Active);
    std::printf("popup vertices %d, height %.0f\n",popup->DrawList->VtxBuffer.Size,popup->ContentSize.y);
    CHECK(popup->DrawList->VtxBuffer.Size>100 && popup->ContentSize.y<500);
    CHECK(window->Size.x<=800);
    CHECK(!(window->Flags&ImGuiWindowFlags_NoMove));
    capture("combo");
    click({popup->DC.CursorStartPos.x+70,popup->DC.CursorStartPos.y+text+style.ItemSpacing.y+text*.5f});
    menu.commit(); CHECK(live.options().render_scale==1 && !live.error());
    CHECK(context.OpenPopupStack.empty());
    const float stride=height+text+3*style.ItemSpacing.y+std::floor(6*unit);
    for(int row:{1,2,3}) {   // FPS, upscaler and quality share the scale combo's full-width column.
        click({graphics->Pos.x+70,graphics->Pos.y+first+row*stride+height*.5f});
        CHECK(popup->Active && popup->DrawList->VtxBuffer.Size>50 && popup->ContentSize.y<500);
        io.AddKeyEvent(ImGuiKey_Escape,true); frame();
        io.AddKeyEvent(ImGuiKey_Escape,false); frame(); frame();
        CHECK(context.OpenPopupStack.empty());
    }
    const float checkbox=first+3*stride+height+2*style.ItemSpacing.y+std::floor(6*unit);
    const float small_height=text;
    CHECK(small_height==29 && std::abs(height-57*.75f)<.01f);   // Fonts and checkbox squares retain their previous 1440p size.
    io.AddMousePosEvent(graphics->Pos.x+small_height*.5f,graphics->Pos.y+checkbox+small_height*.5f);frame();
    CHECK(context.HoveredId==graphics->GetID("##sharpening"));
    click(io.MousePos);
    menu.commit(); CHECK(live.options().sharpening && !live.error());
    click({graphics->Pos.x+graphics->WorkRect.GetWidth()*.75f*.75f,graphics->Pos.y+checkbox+small_height+style.ItemSpacing.y+height*.5f});
    menu.commit(); CHECK(live.options().sharpness>.65 && !live.error());
    capture("rcas");
    const float generation=checkbox+small_height+height+text+4*style.ItemSpacing.y+std::floor(6*unit);
    io.AddMousePosEvent(graphics->InnerClipRect.GetCenter().x,graphics->InnerClipRect.GetCenter().y); frame();
    io.AddMouseWheelEvent(0,-4); frame(); frame();
    click({graphics->Pos.x+70,graphics->Pos.y+generation+height*.5f-graphics->Scroll.y});
    CHECK(popup->Active && popup->DrawList->VtxBuffer.Size>50 && popup->ContentSize.y<500);
    io.AddKeyEvent(ImGuiKey_Escape,true); frame();
    io.AddKeyEvent(ImGuiKey_Escape,false); frame(); frame();
    click({tabs->BarRect.Min.x+tabs->Tabs[1].Offset+tabs->Tabs[1].Width*.5f,tabs->BarRect.GetCenter().y});
    CHECK(tabs->SelectedTabId==tabs->Tabs[1].ID);
    capture("controls");
    {   // the Effects tab: pick 4x anisotropic filtering through the combo
        click({tabs->BarRect.Min.x+tabs->Tabs[2].Offset+tabs->Tabs[2].Width*.5f,tabs->BarRect.GetCenter().y});
        CHECK(tabs->SelectedTabId==tabs->Tabs[2].ID);
        ImGuiWindow* effects=nullptr;
        for(auto* candidate:context.Windows) if(candidate->ParentWindow==window && (candidate->Flags&ImGuiWindowFlags_ChildWindow) && std::string_view(candidate->Name).find("effects")!=std::string_view::npos) effects=candidate;
        CHECK(effects);
        capture("effects");
        CHECK(live.options().effects.anisotropy==16);
        click({effects->Pos.x+40,effects->Pos.y+first+height*.5f});
        popup=ImGui::FindWindowByName("##Combo_00"); CHECK(popup && popup->Active);
        click({popup->DC.CursorStartPos.x+70,popup->DC.CursorStartPos.y+2*(text+style.ItemSpacing.y)+text*.5f});
        menu.commit(); CHECK(live.options().effects.anisotropy==4 && !live.error());
        CHECK(context.OpenPopupStack.empty());
        CHECK(!live.options().effects.per_pixel_lighting);
        io.AddMousePosEvent(effects->Pos.x+small_height*.5f,effects->Pos.y+first+height+2*style.ItemSpacing.y+std::floor(6*unit)+small_height*.5f); frame();
        CHECK(context.HoveredId==effects->GetID("##ppl"));
        click(io.MousePos);
        menu.commit(); CHECK(live.options().effects.per_pixel_lighting && !live.error());
        click({tabs->BarRect.Min.x+tabs->Tabs[1].Offset+tabs->Tabs[1].Width*.5f,tabs->BarRect.GetCenter().y});   // back to Controls for the drag below
        CHECK(tabs->SelectedTabId==tabs->Tabs[1].ID);
    }
    const ImVec2 before=window->Pos;
    io.AddMousePosEvent(before.x+60,before.y+35); frame();
    io.AddMouseButtonEvent(0,true); frame();
    io.AddMousePosEvent(before.x+140,before.y+85); frame(); frame();
    io.AddMouseButtonEvent(0,false); frame(); frame();
    CHECK(window->Pos.x>before.x+50 && window->Pos.y>before.y+25);
    capture("dragged");
    std::puts("Settings menu passed: stable scale, tabs, dropdown selection, RCAS save, drag");
}

int main(int argc,char** argv) {
    const bool gpu=argc==4 && std::string_view(argv[1])=="--gpu";
    const auto folder=std::filesystem::temp_directory_path()/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(folder);
    uc::ConfigDocument document; uc::Options options; options.upscaler=uc::UpscalerKind::dlss;
    uc::store_options(document,options); uc::save_config(folder/"config.json",document);
    uc::SettingsTargets targets;
    targets.render_scale=[](double){}; targets.upscaling=[](const uc::Options&){};
    targets.fps_limit=[](std::optional<uint32_t>){}; targets.bloom=[](bool){}; targets.effects=[](const uc::GraphicsEffects&){};
    targets.prompt_style=[](uc::PromptStyle){}; targets.bindings=[](const uc::Bindings&){};
    uc::LiveSettings live(folder/"config.json",document,targets);
    uc::FeatureReport features; features.dlss=features.dlss_g=features.fsr=features.fsr_fg=true; features.effects=true; features.anisotropy_max=16;
    uc::SettingsMenu menu(live,"ru",[&]{return &features;},2560,1440);
    if(gpu) {
        std::filesystem::create_directories(argv[3]);
        CHECK(SDL_Init(SDL_INIT_VIDEO));
        auto* window=SDL_CreateWindow("Settings menu test",2560,1440,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN); CHECK(window);
        uc::VulkanRenderer renderer; renderer.initialize(window,false,argv[2]);
        ImGuiContext* context=nullptr;
        renderer.set_menu([&]{menu.build();},[&]{menu.setup();context=ImGui::GetCurrentContext();});
        gx::FramePacket packet;
        const auto frame=[&] {
            ++packet.serial; CHECK(renderer.render(packet));
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            bool completed=false;
            while(!completed && std::chrono::steady_clock::now()<deadline) {
                renderer.wait_for_pending(1000000);
                for(auto serial:renderer.poll_completions()) {CHECK(serial==packet.serial && !completed);completed=true;}
            }
            CHECK(completed); ImGui::SetCurrentContext(context);
        };
        exercise(menu,live,frame,[&](const char* name){uc::save_png(renderer.capture(packet.serial),std::filesystem::path(argv[3])/(std::string(name)+".png"));});
        renderer.shutdown(); SDL_DestroyWindow(window); SDL_Quit();
    } else {
        ImGui::CreateContext(); auto& io=ImGui::GetIO();
        io.DisplaySize={2560,1440}; io.DeltaTime=1.f/60; io.IniFilename=nullptr;
        io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
        io.BackendFlags|=ImGuiBackendFlags_RendererHasTextures; menu.setup();
        exercise(menu,live,[&] {
            ImGui::NewFrame(); menu.build(); ImGui::Render();
            for(auto* texture:ImGui::GetPlatformIO().Textures) {texture->SetTexID(1); texture->SetStatus(ImTextureStatus_OK);}
        },[](const char*){});
        ImGui::DestroyContext();
    }
    const auto saved=uc::options_from_config(uc::load_config(folder/"config.json"));
    CHECK(saved.render_scale==1 && saved.sharpening && saved.sharpness>.65 && saved.effects.anisotropy==4);
    std::filesystem::remove(folder/"config.json"); std::filesystem::remove(folder);
}
