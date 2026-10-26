#include "check.h"
#include "update_ui.h"
#include "localization.h"
#include "ui_fonts.h"
#include "graphite.h"
#include <intrin.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <nlohmann/json.hpp>
#include <fstream>
using namespace uc::update;
using namespace uc::launcher;
struct Fake:Transport {
    std::atomic<bool> downloading=false,fail_download=false;
    bool invalid_digest=false;
    Result<HttpResponse> get_metadata(std::stop_token) override {
        return HttpResponse{200,nlohmann::json{{"tag_name","0.0.2"},{"draft",false},{"prerelease",false},
            {"body","Release notes\n\nA complete client update. Settings and saves are preserved.\n<plain text, not HTML>"},
            {"assets",nlohmann::json::array({{{"name","UC-Windows-0.0.2.zip"},{"state","uploaded"},{"size",100},
                {"digest",invalid_digest?"invalid":"sha256:"+std::string(64,'0')},{"browser_download_url","https://github.com/solleo95/reuc-recomp/releases/download/0.0.2/UC-Windows-0.0.2.zip"}}})}}.dump()};
    }
    Status download(const Release&,const DownloadSink&,std::stop_token stop) override {
        downloading=true;
        if(fail_download) return Error{ErrorCode::network,"Fixture connection failed"};
        while(!stop.stop_requested()) Sleep(1);
        return Error{ErrorCode::cancelled,"cancelled"};
    }
};
int main(int argc,char** argv) {
    bool gpu=argc==3 && std::string_view(argv[1])=="--gpu";
    auto root=fs::temp_directory_path()/("uc-update-ui-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    fs::create_directories(root/".uc-update");
    SDL_Window* native=nullptr;SDL_Renderer* renderer=nullptr;
    if(gpu) {
        CHECK(SDL_Init(SDL_INIT_VIDEO));native=SDL_CreateWindow("UC update UI test",1280,720,SDL_WINDOW_HIDDEN);CHECK(native);
        renderer=SDL_CreateRenderer(native,nullptr);CHECK(renderer);fs::create_directories(argv[2]);
    }
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={1280,720};io.DeltaTime=1.f/60;
    io.ConfigInputTrickleEventQueue=false;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    graphite_style();auto fonts=uc::ui::load_fonts(*io.Fonts);
    if(gpu) {CHECK(ImGui_ImplSDL3_InitForSDLRenderer(native,renderer));CHECK(ImGui_ImplSDLRenderer3_Init(renderer));}
    else io.BackendFlags|=ImGuiBackendFlags_RendererHasTextures;
    for(auto language:languages()) {
        auto transport=std::make_unique<Fake>();auto* fake=transport.get();UpdateController controller(root,std::move(transport));
        controller.start(Manifest{{0,0,1},{},{}});bool running=false,capture_next=false;
        auto* current=&controller;
        auto frame=[&] {
            current->poll();
            if(gpu) {SDL_Event event;while(SDL_PollEvent(&event)) {} ImGui_ImplSDLRenderer3_NewFrame();ImGui_ImplSDL3_NewFrame();}
            io.AddFocusEvent(true);ImGui::NewFrame();ImGui::PushFont(fonts.sans,22);
            draw_update_modal(*current,language.code,running);ImGui::PopFont();ImGui::Render();
            if(gpu) {
                SDL_SetRenderDrawColor(renderer,15,17,20,255);CHECK(SDL_RenderClear(renderer));
                ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),renderer);
                if(capture_next) {
                    auto* pixels=SDL_RenderReadPixels(renderer,nullptr);CHECK(pixels);
                    auto path=fs::path(argv[2])/(std::string(language.code)+".bmp");CHECK(SDL_SaveBMP(pixels,path.string().c_str()));SDL_DestroySurface(pixels);capture_next=false;
                }
                CHECK(SDL_RenderPresent(renderer));
            } else for(auto* texture:ImGui::GetPlatformIO().Textures) {texture->SetTexID(1);texture->SetStatus(ImTextureStatus_OK);}
        };
        for(int i=0;i<1000 && controller.busy();++i) {frame();Sleep(1);}frame();frame();
        auto title=std::string(text(language.code,"update_title"))+"###client-update";
        auto* popup=ImGui::FindWindowByName(title.c_str());CHECK(popup && popup->Active);
        CHECK(popup->Pos.x>=0 && popup->Pos.y>=0 && popup->Pos.x+popup->Size.x<=1280 && popup->Pos.y+popup->Size.y<=720);
        auto click=[&](std::string_view key,bool second=false) {
            frame();frame();
            float x=popup->Pos.x+popup->WindowPadding.x+8;
            if(second) {
                ImGui::PushFont(fonts.sans,22);
                x+=ImGui::CalcTextSize(text(language.code,"update_action").data()).x+ImGui::GetStyle().FramePadding.x*2+ImGui::GetStyle().ItemSpacing.x;
                ImGui::PopFont();
            }
            const float y=popup->DC.CursorPosPrevLine.y+8;
            io.AddMousePosEvent(x,y);frame();
            if(!running && ImGui::GetCurrentContext()->HoveredId!=popup->GetID(text(language.code,key).data()))
                std::fprintf(stderr,"%s %s at %.0f,%.0f popup %.0f,%.0f %.0fx%.0f hover=%u expected=%u state=%d\n",language.code.data(),key.data(),x,y,popup->Pos.x,popup->Pos.y,popup->Size.x,popup->Size.y,ImGui::GetCurrentContext()->HoveredId,popup->GetID(text(language.code,key).data()),int(controller.state()));
            if(!running) CHECK(ImGui::GetCurrentContext()->HoveredId==popup->GetID(text(language.code,key).data()));
            io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();frame();
        };
        if(gpu) {capture_next=true;frame();}
        running=true;click("update_action");CHECK(!controller.blocks_game_launch());running=false;
        click("update_action");CHECK(controller.blocks_game_launch());
        for(int i=0;i<1000 && !fake->downloading;++i) Sleep(1);CHECK(fake->downloading);
        click("update_cancel");for(int i=0;i<1000 && controller.busy();++i) {frame();Sleep(1);}
        CHECK(controller.state()==UpdateState::available);CHECK(!controller.blocks_game_launch());
        fake->fail_download=true;click("update_action");
        for(int i=0;i<1000 && controller.busy();++i) {frame();Sleep(1);}
        CHECK(controller.state()==UpdateState::failed);CHECK(!controller.blocks_game_launch());
        fake->fail_download=false;click("update_retry");CHECK(controller.blocks_game_launch());
        click("update_cancel");for(int i=0;i<1000 && controller.busy();++i) {frame();Sleep(1);}
        CHECK(controller.state()==UpdateState::available);
        click("update_later",true);CHECK(controller.state()==UpdateState::disabled);
        frame();CHECK(ImGui::GetCurrentContext()->OpenPopupStack.empty());
        auto invalid=std::make_unique<Fake>();invalid->invalid_digest=true;
        UpdateController broken(root,std::move(invalid));current=&broken;broken.start(Manifest{{0,0,1},{},{}});
        for(int i=0;i<1000 && broken.busy();++i) {frame();Sleep(1);}frame();frame();
        CHECK(broken.state()==UpdateState::failed);CHECK(popup->Active);CHECK(!broken.release());
        click("update_later");frame();CHECK(ImGui::GetCurrentContext()->OpenPopupStack.empty());
    }
    if(gpu) {ImGui_ImplSDLRenderer3_Shutdown();ImGui_ImplSDL3_Shutdown();SDL_DestroyRenderer(renderer);SDL_DestroyWindow(native);SDL_Quit();}
    ImGui::DestroyContext();fs::remove_all(root);
    std::puts("update UI: 13 languages, disabled Update, download, Cancel, error, Retry and Later passed");
}
