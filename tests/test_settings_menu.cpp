#include "check.h"
#include "settings_menu.h"
#include "localization.h"
#include "vulkan_renderer.h"
#include <intrin.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <cstdio>
#include <chrono>
#include <cmath>

static void exercise(uc::SettingsMenu& menu,uc::LiveSettings& live,uc::FeatureReport& features,const std::function<void()>& frame,const std::function<void(const char*)>& capture) {
    for(int i=0;i<4;++i) frame();
    const auto spacing=ImGui::GetStyle().ItemInnerSpacing.x;
    for(int i=0;i<120;++i) frame();
    CHECK(ImGui::GetStyle().ItemInnerSpacing.x==spacing);
    auto* window=ImGui::FindWindowByName("##settings"); CHECK(window);
    CHECK(std::abs(window->Size.x-880)<2 && std::abs(window->Size.y-1200)<2);
    auto& context=*ImGui::GetCurrentContext();
    CHECK(context.TabBars.GetAliveCount()==1);
    auto* tabs=context.TabBars.GetByIndex(0);
    std::printf("tabs %d, menu %.0fx%.0f\n",tabs->Tabs.Size,window->Size.x,window->Size.y);
    CHECK(tabs->Tabs.Size==6);   // Main, Effects, DLSS 5, Controls, Controllers, Reticle
    CHECK((tabs->Flags&ImGuiTabBarFlags_FittingPolicyScroll)!=0);   // more tabs than the width (mods) scroll instead of shrinking
    CHECK(tabs->Tabs[3].Offset+tabs->Tabs[3].Width<=tabs->BarRect.GetWidth());
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
    CHECK(window->Size.x<=900);
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
    features.dlss=features.dlss_g=false;
    frame();menu.commit();
    CHECK(live.options().upscaler==uc::UpscalerKind::off && !live.options().frame_generation);
    click({graphics->Pos.x+70,graphics->Pos.y+first+2*stride+height*.5f});
    CHECK(popup->Active);
    click({popup->DC.CursorStartPos.x+70,popup->DC.CursorStartPos.y+2*(text+style.ItemSpacing.y)+text*.5f});
    menu.commit();CHECK(live.options().upscaler==uc::UpscalerKind::off);   // clicking unsupported DLSS cannot restore it
    capture("unsupported-dlss");
    io.AddKeyEvent(ImGuiKey_Escape,true);frame();
    io.AddKeyEvent(ImGuiKey_Escape,false);frame();menu.commit();
    features.dlss=features.dlss_g=true;
    auto restored=live.options();restored.upscaler=uc::UpscalerKind::dlss;live.apply(restored);
    frame();
    const float generation=checkbox+small_height+height+text+4*style.ItemSpacing.y+std::floor(6*unit);
    io.AddMousePosEvent(graphics->InnerClipRect.GetCenter().x,graphics->InnerClipRect.GetCenter().y); frame();
    io.AddMouseWheelEvent(0,-4); frame(); frame();
    click({graphics->Pos.x+70,graphics->Pos.y+generation+height*.5f-graphics->Scroll.y});
    CHECK(popup->Active && popup->DrawList->VtxBuffer.Size>50 && popup->ContentSize.y<500);
    io.AddKeyEvent(ImGuiKey_Escape,true); frame();
    io.AddKeyEvent(ImGuiKey_Escape,false); frame(); frame();
    click({tabs->BarRect.Min.x+tabs->Tabs[3].Offset+tabs->Tabs[3].Width*.5f,tabs->BarRect.GetCenter().y});
    CHECK(tabs->SelectedTabId==tabs->Tabs[3].ID);
    capture("controls");
    {   // The last camera row remains reachable and saves a captured replacement binding.
        ImGuiWindow* bindings=nullptr;
        for(auto* candidate:context.Windows) if(candidate->Active && std::string_view(candidate->Name).find("bindings")!=std::string_view::npos) bindings=candidate;
        CHECK(bindings);
        io.AddMousePosEvent(bindings->InnerClipRect.GetCenter().x,bindings->InnerClipRect.GetCenter().y); frame();
        io.AddMouseWheelEvent(0,-100); frame(); frame();
        bindings->IDStack.push_back(bindings->GetID(int(uc::Action::camera_right)));
        const auto button=bindings->GetID(SDL_GetScancodeName(SDL_SCANCODE_RIGHT));
        bindings->IDStack.pop_back();
        bool found=false;
        for(float y=bindings->InnerClipRect.Min.y+4;y<bindings->InnerClipRect.Max.y;y+=3) {
            io.AddMousePosEvent(bindings->Pos.x+70,y); frame();
            if(context.HoveredId==button) {click(io.MousePos);found=true;break;}
        }
        CHECK(found);
        SDL_Event key{}; key.type=SDL_EVENT_KEY_DOWN; key.key.scancode=SDL_SCANCODE_F6; key.key.down=true;
        menu.capture_event(key); menu.commit(); frame();
        const auto& entries=live.bindings()[size_t(uc::Action::camera_right)];
        CHECK(entries.size()==1 && entries[0].input.kind==uc::PhysicalKind::key && entries[0].input.code==SDL_SCANCODE_F6 && !live.error());
        capture("camera-bindings");
    }
    {   // The Controllers tab: the Gamepad button opens a window with the gamepad's own bindings; a pad press captures one
        tabs->NextSelectedTabId=tabs->Tabs[4].ID; frame(); frame();   // the last tab scrolls into view in a narrow menu (like the mod tabs): it is selected directly here
        CHECK(tabs->SelectedTabId==tabs->Tabs[4].ID);
        capture("controllers");
        window->IDStack.push_back(tabs->ID); window->IDStack.push_back(tabs->Tabs[4].ID);   // the tab bar and the selected tab are on the ID stack of the tab's widgets
        const auto gamepad_button=window->GetID(std::string(uc::launcher::text("ru","gamepad")).c_str());   // the menu runs in Russian here
        window->IDStack.pop_back(); window->IDStack.pop_back();
        bool found=false;
        for(float y=window->Pos.y+60;y<window->Pos.y+window->Size.y-20;y+=3) {
            io.AddMousePosEvent(window->Pos.x+90,y); frame();
            if(context.HoveredId==gamepad_button) {click(io.MousePos);found=true;break;}
        }
        CHECK(found);
        frame(); frame();
        ImGuiWindow* modal=nullptr;
        for(auto* candidate:context.Windows) if(candidate->Active && std::string_view(candidate->Name).find("gamepad-modal")!=std::string_view::npos) modal=candidate;
        CHECK(modal);
        capture("gamepad-window");
        ImGuiWindow* pad_bindings=nullptr;
        for(auto* candidate:context.Windows) if(candidate->Active && std::string_view(candidate->Name).find("pad-body")!=std::string_view::npos) pad_bindings=candidate;
        CHECK(pad_bindings);
        pad_bindings->IDStack.push_back(pad_bindings->GetID(int(uc::Action::interact)));
        const auto interact_button=pad_bindings->GetID("##bind");   // the binding is drawn as the controller's picture over this button
        pad_bindings->IDStack.pop_back();
        // The binding rows sit below the sticks and the gyro settings: scroll the window's body until the first row's button is under the pointer, then click it.
        const auto click_binding=[&](ImGuiID id) {
            io.AddMousePosEvent(pad_bindings->InnerClipRect.GetCenter().x,pad_bindings->InnerClipRect.GetCenter().y); frame();
            for(int attempt=0;attempt<10;++attempt) {
                for(float y=pad_bindings->InnerClipRect.Min.y+4;y<pad_bindings->InnerClipRect.Max.y;y+=3) {
                    io.AddMousePosEvent(pad_bindings->InnerClipRect.Min.x+pad_bindings->InnerClipRect.GetWidth()*.6f,y); frame();
                    if(context.HoveredId==id) {click(io.MousePos);return true;}
                }
                io.AddMouseWheelEvent(0,-3); frame(); frame();
            }
            return false;
        };
        found=click_binding(interact_button);
        CHECK(found);
        menu.pad_capture({uc::PhysicalKind::pad_button,3},true); frame(); menu.commit(); frame();
        const auto& entries=live.options().gamepad.bindings[size_t(uc::Action::interact)];
        CHECK(entries.size()==1 && entries[0].input==(uc::PhysicalInput{uc::PhysicalKind::pad_button,3}) && !live.error());
        CHECK(live.bindings()[size_t(uc::Action::interact)]==uc::default_bindings()[size_t(uc::Action::interact)]);   // the keyboard table is untouched
        capture("gamepad-bindings");
        {   // gamepad bindings changed from outside (the harness, a script) are shown, not reverted by the tab's own stale editor
            auto next=live.options(); next.gamepad.bindings[size_t(uc::Action::fire)].clear(); live.apply(next);
            for(int i=0;i<3;++i) frame();
            menu.commit(); frame(); menu.commit();
            CHECK(live.options().gamepad.bindings[size_t(uc::Action::fire)].empty() && !live.error());
        }
        {   // the menu closing (Esc or Insert reach the host, not the window) cancels a capture waiting for a pad press: nothing is rebound afterwards
            pad_bindings->IDStack.push_back(pad_bindings->GetID(int(uc::Action::interact)));
            const auto again=pad_bindings->GetID("##bind");
            pad_bindings->IDStack.pop_back();
            found=click_binding(again);
            CHECK(found);
            menu.menu_closed();
            menu.pad_capture({uc::PhysicalKind::pad_button,5},true); frame(); menu.commit(); frame(); menu.commit();
            const auto& after=live.options().gamepad.bindings[size_t(uc::Action::interact)];
            CHECK(after.size()==1 && after[0].input==(uc::PhysicalInput{uc::PhysicalKind::pad_button,3}));
        }
        io.AddKeyEvent(ImGuiKey_Escape,true); frame(); io.AddKeyEvent(ImGuiKey_Escape,false); frame();
        tabs->NextSelectedTabId=tabs->Tabs[0].ID; for(int i=0;i<40;++i) frame();   // back to the first tab: the bar scrolls home for the clicks below
    }
    {   // the Effects tab: pick 4x anisotropic filtering through the combo
        click({tabs->BarRect.Min.x+tabs->Tabs[1].Offset+tabs->Tabs[1].Width*.5f,tabs->BarRect.GetCenter().y});
        CHECK(tabs->SelectedTabId==tabs->Tabs[1].ID);
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
        CHECK(live.options().effects.ambient_occlusion==uc::EffectLevel::off);
        {   // ambient occlusion: the preset combo below the checkbox, pick "high" (the fourth entry)
            const float checkbox_top=first+height+2*style.ItemSpacing.y+std::floor(6*unit);
            const float combo_center=checkbox_top+small_height+style.ItemSpacing.y+std::floor(6*unit)+style.ItemSpacing.y+text+style.ItemSpacing.y+height*.5f;
            click({effects->Pos.x+40,effects->Pos.y+combo_center});
            popup=ImGui::FindWindowByName("##Combo_00"); CHECK(popup && popup->Active);
            click({popup->DC.CursorStartPos.x+70,popup->DC.CursorStartPos.y+3*(text+style.ItemSpacing.y)+text*.5f});
            menu.commit(); CHECK(live.options().effects.ambient_occlusion==uc::EffectLevel::high && !live.error());
            CHECK(context.OpenPopupStack.empty());
            CHECK(live.options().effects.motion_blur==uc::EffectLevel::off);
            const float blur_center=combo_center+height+3*style.ItemSpacing.y+std::floor(6*unit)+text;   // the motion blur combo follows the occlusion one
            click({effects->Pos.x+40,effects->Pos.y+blur_center});
            popup=ImGui::FindWindowByName("##Combo_00"); CHECK(popup && popup->Active);
            click({popup->DC.CursorStartPos.x+70,popup->DC.CursorStartPos.y+1*(text+style.ItemSpacing.y)+text*.5f});
            menu.commit(); CHECK(live.options().effects.motion_blur==uc::EffectLevel::low && !live.error());
            CHECK(context.OpenPopupStack.empty());
            CHECK(!live.options().effects.contact_shadows);
            io.AddMousePosEvent(effects->Pos.x+small_height*.5f,effects->Pos.y+blur_center+height*.5f+2*style.ItemSpacing.y+std::floor(6*unit)+small_height*.5f); frame();
            CHECK(context.HoveredId==effects->GetID("##contact"));
            click(io.MousePos);
            menu.commit(); CHECK(live.options().effects.contact_shadows && !live.error());
        }
        {   // the DLSS 5 tab: the switch is the first control, then one control per setting; it saves through the dlss5 target
            click({tabs->BarRect.Min.x+tabs->Tabs[2].Offset+tabs->Tabs[2].Width*.5f,tabs->BarRect.GetCenter().y});
            CHECK(tabs->SelectedTabId==tabs->Tabs[2].ID);
            ImGuiWindow* dlss5=nullptr;
            for(auto* candidate:context.Windows) if(candidate->ParentWindow==window && (candidate->Flags&ImGuiWindowFlags_ChildWindow) && std::string_view(candidate->Name).find("dlss5")!=std::string_view::npos) dlss5=candidate;
            CHECK(dlss5);
            capture("dlss5");
            CHECK(!live.options().dlss5.enabled());
            // the Hook Method bar is the second row (under the Preset selector): its last button is Present, its first Off
            const float control=std::floor(dlss5->WorkRect.GetWidth()*.56f);
            const auto button=[&](int index,const char* label) {   // the id of a bar button: row "hook", button index, label
                dlss5->IDStack.push_back(dlss5->GetID("hook")); dlss5->IDStack.push_back(dlss5->GetID(index));
                const ImGuiID id=dlss5->GetID(label); dlss5->IDStack.pop_back(); dlss5->IDStack.pop_back(); return id;
            };
            const auto find_row=[&](float x,ImGuiID id) {   // the y at which the mouse hovers that button
                for(float y=dlss5->Pos.y+4;y<dlss5->Pos.y+dlss5->Size.y;y+=3) { io.AddMousePosEvent(x,y); frame(); if(context.HoveredId==id) return y; }
                return -1.f;
            };
            const float bar_y=find_row(dlss5->Pos.x+control-12,button(3,"Present")); CHECK(bar_y>0);
            click({dlss5->Pos.x+control-12,bar_y});
            menu.commit(); CHECK(live.options().dlss5.hook==uc::Dlss5Hook::present && !live.error());
            capture("dlss5-on");
            // this run's refusal (the renderer's reason) greys the rows but leaves the Hook Method bar usable to turn it off again
            menu.set_dlss5_status([] { return uc::SettingsMenu::Dlss5Report{false,"nvngx_dlssnr.dll missing next to the game"}; });
            for(int i=0;i<3;++i) frame();
            capture("dlss5-unavailable");
            click({dlss5->Pos.x+12,bar_y});
            menu.commit(); CHECK(live.options().dlss5.hook==uc::Dlss5Hook::off && !live.error());
            menu.set_dlss5_status({});
            // an Intensity slider reset: a changed value is restored by its reset button (the arrow beside the label), and the button is greyed at the default
            { menu.commit(); uc::Options changed=live.options(); changed.dlss5.intensity=1.5; live.apply(changed); for(int i=0;i<3;++i) frame(); CHECK(live.options().dlss5.intensity==1.5);
              dlss5->IDStack.push_back(dlss5->GetID("intensity")); const ImGuiID reset=dlss5->GetID("##reset-intensity"); dlss5->IDStack.pop_back();
              const float reset_x=dlss5->Pos.x+control+2*style.ItemInnerSpacing.x+height*.5f, reset_y=find_row(reset_x,reset); CHECK(reset_y>0);
              click({reset_x,reset_y}); menu.commit(); CHECK(live.options().dlss5.intensity==1 && !live.error()); }
        }
        click({tabs->BarRect.Min.x+tabs->Tabs[3].Offset+tabs->Tabs[3].Width*.5f,tabs->BarRect.GetCenter().y});   // back to Controls for the drag below
        CHECK(tabs->SelectedTabId==tabs->Tabs[3].ID);
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

int main(int argc,char** argv) {    const bool gpu=argc==4 && std::string_view(argv[1])=="--gpu";
    const auto folder=std::filesystem::temp_directory_path()/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(folder);
    uc::ConfigDocument document; uc::Options options; options.upscaler=uc::UpscalerKind::dlss; options.dlss5_enabled=true;
    uc::store_options(document,options); uc::save_config(folder/"config.json",document);
    uc::SettingsTargets targets;
    targets.render_scale=[](double){}; targets.upscaling=[](const uc::Options&){};
    targets.fps_limit=[](std::optional<uint32_t>){}; targets.bloom=[](const uc::BloomSettings&){}; targets.crosshair_scale=[](uint32_t){}; targets.reticle=[](const uc::ReticleOptions&){}; targets.mouse_sensitivity=[](uint32_t){}; targets.hud_position=[](uc::HudPosition){}; targets.effects=[](const uc::GraphicsEffects&){};
    targets.dlss5=[](const uc::Dlss5Settings&){}; targets.prompt_style=[](uc::PromptStyle){}; targets.bindings=[](const uc::Bindings&){}; targets.gamepad=[](const uc::GamepadSettings&){};
    uc::LiveSettings live(folder/"config.json",document,targets);
    uc::FeatureReport features; features.dlss=features.dlss_g=features.fsr=features.fsr_fg=true; features.effects=true; features.anisotropy_max=16; features.dlss5=true;
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
        exercise(menu,live,features,frame,[&](const char* name){uc::save_png(renderer.capture(packet.serial),std::filesystem::path(argv[3])/(std::string(name)+".png"));});
        renderer.shutdown(); SDL_DestroyWindow(window); SDL_Quit();
    } else {
        ImGui::CreateContext(); auto& io=ImGui::GetIO();
        io.DisplaySize={2560,1440}; io.DeltaTime=1.f/60; io.IniFilename=nullptr;
        io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
        io.BackendFlags|=ImGuiBackendFlags_RendererHasTextures; menu.setup();
        exercise(menu,live,features,[&] {
            ImGui::NewFrame(); menu.build(); ImGui::Render();
            for(auto* texture:ImGui::GetPlatformIO().Textures) {texture->SetTexID(1); texture->SetStatus(ImTextureStatus_OK);}
        },[](const char*){});
        ImGui::DestroyContext();
    }
    const auto saved=uc::options_from_config(uc::load_config(folder/"config.json"));
    CHECK(saved.render_scale==1 && saved.sharpening && saved.sharpness>.65 && saved.effects.anisotropy==4 && saved.effects.per_pixel_lighting && saved.effects.ambient_occlusion==uc::EffectLevel::high && saved.effects.motion_blur==uc::EffectLevel::low && saved.effects.contact_shadows);
    std::filesystem::remove(folder/"config.json"); std::filesystem::remove(folder);
}
