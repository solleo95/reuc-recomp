#include "check.h"
#include "deck_menu.h"
#include "vulkan_renderer.h"
#include <intrin.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <chrono>
#include <cstdio>
#include <filesystem>

static SDL_Event pad_button(SDL_GamepadButton button,bool down) {
    SDL_Event event{}; event.type=down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP; event.gbutton.button=uint8_t(button); event.gbutton.down=down; return event;
}
static SDL_Event pad_axis(SDL_GamepadAxis axis,int16_t value) {
    SDL_Event event{}; event.type=SDL_EVENT_GAMEPAD_AXIS_MOTION; event.gaxis.axis=uint8_t(axis); event.gaxis.value=value; return event;
}

static void exercise(uc::DeckMenu& menu,uc::LiveSettings& live,const std::function<void()>& frame,const std::function<void(const char*)>& capture,bool& closed) {
    const auto tap=[&](uint32_t button) { auto down=pad_button(SDL_GamepadButton(button),true), up=pad_button(SDL_GamepadButton(button),false); CHECK(menu.event(down)); frame(); CHECK(menu.event(up)); frame(); frame(); };
    menu.opened();
    for(int i=0;i<4;++i) frame();
    auto& io=ImGui::GetIO(); auto& context=*ImGui::GetCurrentContext();   // the renderer creates the context with its first frame
    auto* panel=ImGui::FindWindowByName("##deck_menu"); CHECK(panel);
    CHECK(std::abs(panel->Size.x-1180)<2 && std::abs(panel->Size.y-660)<2);   // at 1280x720 the panel is 1180x660
    capture("graphics");
    {   // the right trackpad pressed firmly clicks at the cursor (the trackpad itself only moves it); a light touch does not
        ImGuiWindow* rows=nullptr;
        for(auto* candidate:context.Windows) if(candidate->ParentWindow==panel && (candidate->Flags&ImGuiWindowFlags_ChildWindow) && std::string_view(candidate->Name).find("##rows")!=std::string_view::npos) rows=candidate;
        CHECK(rows);
        io.AddMousePosEvent(rows->Pos.x+80,rows->Pos.y+40); frame(); frame();   // over the first row
        const auto touchpad=[&](SDL_EventType type,float pressure) { SDL_Event e{}; e.type=type; e.gtouchpad.touchpad=1; e.gtouchpad.finger=0; e.gtouchpad.pressure=pressure; menu.event(e); };
        touchpad(SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN,.05f); for(int i=0;i<3;++i) frame();
        CHECK(context.OpenPopupStack.empty());                               // resting a finger is not a click
        touchpad(SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION,.5f); for(int i=0;i<3;++i) frame();
        touchpad(SDL_EVENT_GAMEPAD_TOUCHPAD_UP,0); for(int i=0;i<3;++i) frame();
        CHECK(!context.OpenPopupStack.empty());                              // pressed and let go over the render scale row: its list opened
        tap(SDL_GAMEPAD_BUTTON_EAST);                                         // back closes the list
        CHECK(context.OpenPopupStack.empty());
    }
    {   // a choice with the pad: A opens the list, down moves, A picks; the pick is saved
        CHECK(live.options().render_scale==0);
        tap(SDL_GAMEPAD_BUTTON_SOUTH);
        CHECK(!context.OpenPopupStack.empty());
        capture("list");
        tap(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        tap(SDL_GAMEPAD_BUTTON_SOUTH);
        menu.commit(); CHECK(live.options().render_scale==1 && !live.error());
        CHECK(context.OpenPopupStack.empty());
    }
    {   // the next row, and a value with left/right: the frame rate limit
        tap(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        const auto before=live.options().fps_limit.value_or(0);
        tap(SDL_GAMEPAD_BUTTON_DPAD_RIGHT); menu.commit();
        CHECK(live.options().fps_limit.value_or(0)!=before && !live.error());
    }
    {   // the left stick is a D-pad: pushed up it moves back to the render scale row, where right goes to the next entry
        auto push=pad_axis(SDL_GAMEPAD_AXIS_LEFTY,-30000); CHECK(menu.event(push)); for(int i=0;i<4;++i) frame();
        auto release=pad_axis(SDL_GAMEPAD_AXIS_LEFTY,0); CHECK(menu.event(release)); frame();
        CHECK(live.options().render_scale==1);
        tap(SDL_GAMEPAD_BUTTON_DPAD_RIGHT); menu.commit();
        CHECK(live.options().render_scale==1.5);
    }
    {   // the next page with R1: effects; a switch with A
        tap(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        capture("effects");
        CHECK(!live.options().effects.per_pixel_lighting);
        tap(SDL_GAMEPAD_BUTTON_DPAD_DOWN);   // anisotropy -> per-pixel lighting
        tap(SDL_GAMEPAD_BUTTON_SOUTH); menu.commit();
        CHECK(live.options().effects.per_pixel_lighting && !live.error());
        tap(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    }
    {   // a finger: one tap on a switch row flips it
        tap(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);   // effects again
        ImGuiWindow* rows=nullptr;
        for(auto* candidate:context.Windows) if(candidate->ParentWindow==panel && (candidate->Flags&ImGuiWindowFlags_ChildWindow) && std::string_view(candidate->Name).find("##rows")!=std::string_view::npos) rows=candidate;
        CHECK(rows);
        rows->IDStack.push_back(rows->GetID(1)); const ImGuiID row=rows->GetID("##row"); rows->IDStack.pop_back();   // row 1: per-pixel lighting
        float y=-1; for(float probe=rows->Pos.y+2;probe<rows->Pos.y+rows->Size.y;probe+=3) { io.AddMousePosEvent(rows->Pos.x+80,probe); frame(); if(context.HoveredId==row) { y=probe; break; } }
        CHECK(y>0);
        CHECK(live.options().effects.per_pixel_lighting);
        io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); frame(); frame();
        menu.commit(); CHECK(!live.options().effects.per_pixel_lighting && !live.error());   // one tap, one change
    }
    {   // the close button, and B at the top level, ask the menu to close
        tap(SDL_GAMEPAD_BUTTON_EAST); menu.commit();
        CHECK(closed);
        closed=false;
    }
    {   // the Buttons page: A on an action waits for a pad button, which replaces the bindings; Y clears them; the last row restores the defaults
        tap(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER); tap(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);   // Controls, then Buttons
        capture("buttons");
        const auto interact=size_t(uc::Action::interact);
        tap(SDL_GAMEPAD_BUTTON_SOUTH);   // the first row: interact
        capture("buttons-capture");
        auto stray=pad_button(SDL_GAMEPAD_BUTTON_SOUTH,true); CHECK(menu.event(stray));   // while a capture waits, every pad button is the capture's (not a menu key)
        {   // B itself can be bound: the capture takes it (the pad sink runs first and ends the capture), so it is not also "back"
            menu.pad_capture({uc::PhysicalKind::pad_button,1},true);
            auto b=pad_button(SDL_GAMEPAD_BUTTON_EAST,true); CHECK(menu.event(b));
            auto b_up=pad_button(SDL_GAMEPAD_BUTTON_EAST,false); menu.pad_capture({uc::PhysicalKind::pad_button,1},false); CHECK(menu.event(b_up));
            frame(); frame(); menu.commit();
            CHECK(!closed && live.options().gamepad.bindings[interact].size()==1 && live.options().gamepad.bindings[interact][0].input==(uc::PhysicalInput{uc::PhysicalKind::pad_button,1}));
            tap(SDL_GAMEPAD_BUTTON_SOUTH);   // and the next capture, for the rest of the test
        }
        menu.pad_capture({uc::PhysicalKind::pad_button,7},true); auto stick_down=pad_button(SDL_GAMEPAD_BUTTON_LEFT_STICK,true); CHECK(menu.event(stick_down));   // the sink runs, then the event
        menu.pad_capture({uc::PhysicalKind::pad_button,7},false); auto stick_up=pad_button(SDL_GAMEPAD_BUTTON_LEFT_STICK,false); menu.event(stick_up);   // the release of a button the menu has no use for
        frame(); frame(); menu.commit();
        CHECK(live.options().gamepad.bindings[interact].size()==1 && live.options().gamepad.bindings[interact][0].input==(uc::PhysicalInput{uc::PhysicalKind::pad_button,7}) && !live.error());
        tap(SDL_GAMEPAD_BUTTON_NORTH); menu.commit();   // Y: clear
        CHECK(live.options().gamepad.bindings[interact].empty());
        for(int i=0;i<17;++i) tap(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        tap(SDL_GAMEPAD_BUTTON_SOUTH); menu.commit();   // the last row
        CHECK(live.options().gamepad.bindings==uc::default_pad_bindings());
        {   // the D-pad reaches the plus and the cross of a row: right, A adds a binding; right, A clears them
            for(int i=0;i<17;++i) tap(SDL_GAMEPAD_BUTTON_DPAD_UP);   // back to the first row
            const auto before=live.options().gamepad.bindings[interact].size();
            tap(SDL_GAMEPAD_BUTTON_DPAD_RIGHT); tap(SDL_GAMEPAD_BUTTON_SOUTH);   // the plus: waits for a button
            menu.pad_capture({uc::PhysicalKind::pad_button,3},true); auto north=pad_button(SDL_GAMEPAD_BUTTON_NORTH,true); CHECK(menu.event(north));
            menu.pad_capture({uc::PhysicalKind::pad_button,3},false); auto north_up=pad_button(SDL_GAMEPAD_BUTTON_NORTH,false); menu.event(north_up);
            frame(); frame(); menu.commit();
            CHECK(live.options().gamepad.bindings[interact].size()==before+1);   // added, the old ones stay
            tap(SDL_GAMEPAD_BUTTON_DPAD_RIGHT); tap(SDL_GAMEPAD_BUTTON_SOUTH); menu.commit();   // the cross
            CHECK(live.options().gamepad.bindings[interact].empty());
        }
        tap(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER); tap(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER); tap(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    }
    {   // "while a button is held": a row under the mode waits for the button; only a trigger, a shoulder or a stick button is taken
        tap(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER); tap(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);   // Effects, then Controls
        for(int i=0;i<3 && live.options().gamepad.gyro.mode!=uc::GyroMode::hold;++i) { tap(SDL_GAMEPAD_BUTTON_DPAD_RIGHT); menu.commit(); }   // the mode row is the first
        CHECK(live.options().gamepad.gyro.mode==uc::GyroMode::hold && !live.error());
        capture("gyro-hold");
        tap(SDL_GAMEPAD_BUTTON_DPAD_DOWN);                  // the new row
        tap(SDL_GAMEPAD_BUTTON_SOUTH);                      // A: waits for a button
        capture("gyro-hold-capture");
        const auto before=live.options().gamepad.gyro_hold;
        menu.pad_capture({uc::PhysicalKind::pad_axis,1},true);   // a stick direction cannot hold the gyro: ignored, the row keeps waiting
        auto stick=pad_axis(SDL_GAMEPAD_AXIS_LEFTX,30000); CHECK(menu.event(stick)); frame(); frame(); menu.commit();
        CHECK(live.options().gamepad.gyro_hold==before);
        menu.pad_capture({uc::PhysicalKind::pad_button,0},true);   // any button can: a face button is taken at once (and the row stops waiting)
        auto face=pad_button(SDL_GAMEPAD_BUTTON_SOUTH,true); CHECK(menu.event(face)); frame(); frame(); menu.commit();
        CHECK(live.options().gamepad.gyro_hold==(uc::PhysicalInput{uc::PhysicalKind::pad_button,0}) && !live.error());
        tap(SDL_GAMEPAD_BUTTON_SOUTH);                      // A: waits again, for the trigger below
        menu.pad_capture({uc::PhysicalKind::pad_axis,11},true);    // the right trigger
        auto trigger=pad_axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,30000); CHECK(menu.event(trigger)); frame(); frame(); menu.commit();
        CHECK(live.options().gamepad.gyro_hold==(uc::PhysicalInput{uc::PhysicalKind::pad_axis,11}) && !live.error());
        {   // the recenter row right under the gyro button: A waits for a button, which replaces the binding; the old one is gone
            tap(SDL_GAMEPAD_BUTTON_DPAD_DOWN);   // from the hold button to recenter
            const auto recenter=size_t(uc::Action::recenter);
            tap(SDL_GAMEPAD_BUTTON_SOUTH);
            capture("recenter-capture");
            menu.pad_capture({uc::PhysicalKind::pad_button,3},true); auto north=pad_button(SDL_GAMEPAD_BUTTON_NORTH,true); CHECK(menu.event(north));
            menu.pad_capture({uc::PhysicalKind::pad_button,3},false); auto north_up=pad_button(SDL_GAMEPAD_BUTTON_NORTH,false); menu.event(north_up);
            frame(); frame(); menu.commit();
            CHECK(live.options().gamepad.bindings[recenter].size()==1 && live.options().gamepad.bindings[recenter][0].input==(uc::PhysicalInput{uc::PhysicalKind::pad_button,3}) && !live.error());
            capture("recenter");
        }
        tap(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER); tap(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);   // back to the first page
    }
    {   // the Mods page exists only with a mod platform; its manager is drawn in a window of its own
        struct Fake : uc::mods::ModMenu { int drawn=0; std::string manager_title() override { return "Mods"; } void manager_tab(float) override { ++drawn; ImGui::TextUnformatted("fake mod manager"); } void commit() override {} } fake;
        CHECK(!ImGui::FindWindowByName("##deck_mods"));
        menu.set_mod_menu(&fake);
        for(int i=0;i<5;++i) tap(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);   // Effects, Controls, Buttons, Reticle, Mods (Quit game comes after it)
        for(int i=0;i<3;++i) frame();
        CHECK(fake.drawn>0 && ImGui::FindWindowByName("##deck_mods"));
        capture("mods");
        tap(SDL_GAMEPAD_BUTTON_EAST); menu.commit(); CHECK(closed); closed=false;   // B with nothing open closes the menu
        menu.set_mod_menu(nullptr);
        for(int i=0;i<8;++i) tap(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    }
    {   // a slider with left/right and a colour with A: the reticle page
        for(int i=0;i<4;++i) tap(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);   // Reticle again (no Mods page without a mod platform)
        capture("reticle");
        const auto before=live.options().crosshair_scale;
        tap(SDL_GAMEPAD_BUTTON_DPAD_LEFT); menu.commit();
        CHECK(live.options().crosshair_scale==before-5 && !live.error());
    }
    {   // the last page, Quit game: the first A asks, only a second A at once quits; any other key takes the question back
        int quits=0; menu.set_quit([&] { ++quits; });
        tap(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);   // Quit game
        capture("quit");
        tap(SDL_GAMEPAD_BUTTON_SOUTH); menu.commit();
        CHECK(quits==0);
        capture("quit-ask");
        tap(SDL_GAMEPAD_BUTTON_DPAD_DOWN); tap(SDL_GAMEPAD_BUTTON_SOUTH); menu.commit();   // a key between the two presses: asks again
        CHECK(quits==0);
        tap(SDL_GAMEPAD_BUTTON_SOUTH); menu.commit();
        CHECK(quits==1 && !closed);
    }
    std::puts("Deck menu passed: pages, list, switch, slider, repeat, tap, close");
}

int main(int argc,char** argv) {
    const bool gpu=argc==4 && std::string_view(argv[1])=="--gpu";
    const auto folder=std::filesystem::temp_directory_path()/("deck_menu_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    uc::ConfigDocument document; uc::Options options; uc::store_options(document,options); uc::save_config(folder/"config.json",document);
    uc::SettingsTargets targets;
    targets.render_scale=[](double) {}; targets.upscaling=[](const uc::Options&) {}; targets.fps_limit=[](std::optional<uint32_t>) {}; targets.bloom=[](const uc::BloomSettings&) {};
    targets.crosshair_scale=[](uint32_t) {}; targets.reticle=[](const uc::ReticleOptions&) {}; targets.mouse_sensitivity=[](uint32_t) {}; targets.pointer=[](uc::PointerMode,uint32_t) {};
    targets.reload_zone=[](uc::ReloadZone,uint32_t) {}; targets.hud_position=[](uc::HudPosition) {}; targets.effects=[](const uc::GraphicsEffects&) {}; targets.dlss5=[](const uc::Dlss5Settings&) {};
    targets.prompt_style=[](uc::PromptStyle) {}; targets.hdr=[](const uc::HdrSettings&) {}; targets.bindings=[](const uc::Bindings&) {}; targets.gamepad=[](const uc::GamepadSettings&) {}; targets.wiimote=[](const uc::WiimoteSettings&) {};
    uc::LiveSettings live(folder/"config.json",document,targets);
    uc::FeatureReport features; features.effects=true; features.anisotropy_max=16;
    uc::DeckMenu menu(live,"en",[&] { return &features; },1280,720);
    bool closed=false; menu.set_close([&] { closed=true; });
    if(gpu) {
        std::filesystem::create_directories(argv[3]);
        CHECK(SDL_Init(SDL_INIT_VIDEO));
        auto* window=SDL_CreateWindow("Deck menu test",1280,720,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN); CHECK(window);
        uc::VulkanRenderer renderer; renderer.initialize(window,false,argv[2]);
        ImGuiContext* context=nullptr;
        renderer.set_menu([&] { menu.build(); },[&] { menu.setup(); context=ImGui::GetCurrentContext(); });
        gx::FramePacket packet;
        const auto frame=[&] {
            ++packet.serial; CHECK(renderer.render(packet));
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            bool completed=false;
            while(!completed && std::chrono::steady_clock::now()<deadline) {
                renderer.wait_for_pending(1000000);
                for(auto serial:renderer.poll_completions()) { CHECK(serial==packet.serial && !completed); completed=true; }
            }
            CHECK(completed); ImGui::SetCurrentContext(context);
        };
        exercise(menu,live,frame,[&](const char* name) { uc::save_png(renderer.capture(packet.serial),std::filesystem::path(argv[3])/(std::string(name)+".png")); },closed);
        renderer.shutdown(); SDL_DestroyWindow(window); SDL_Quit();
    } else {
        ImGui::CreateContext(); auto& io=ImGui::GetIO();
        io.DisplaySize={1280,720}; io.DeltaTime=1.f/60; io.IniFilename=nullptr;
        io.BackendFlags|=ImGuiBackendFlags_RendererHasTextures; menu.setup();
        exercise(menu,live,[&] {
            ImGui::NewFrame(); menu.build(); ImGui::Render();
            for(auto* texture:ImGui::GetPlatformIO().Textures) { texture->SetTexID(1); texture->SetStatus(ImTextureStatus_OK); }
        },[](const char*) {},closed);
        ImGui::DestroyContext();
    }
    std::filesystem::remove(folder/"config.json"); std::filesystem::remove(folder);
}
