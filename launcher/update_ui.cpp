#include "update_ui.h"
#include "localization.h"
#include "imgui.h"
#include <algorithm>
#include <SDL3/SDL.h>
namespace uc::launcher {
bool draw_update_modal(UpdateController& controller,std::string_view language,bool game_running,float scale) {
    auto tr=[&](std::string_view key){return text(language,key).data();};
    auto state=controller.state();
    bool visible=(controller.release().has_value() || controller.error().has_value()) && state!=UpdateState::disabled && state!=UpdateState::checking;
    if(!visible) return false;
    const auto title=std::string(tr("update_title"))+"###client-update";
    if(!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)) ImGui::OpenPopup(title.c_str());
    auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize({std::min(660*scale,display.x-32),0},ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints({0,0},{display.x-32,display.y-32});
    ImGui::SetNextWindowPos({display.x/2,display.y/2},ImGuiCond_Always,{.5f,.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{28*scale,26*scale});
    bool accepted=false;
    if(ImGui::BeginPopupModal(title.c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoTitleBar)) {
        const float font_size=ImGui::GetFontSize();
        ImGui::PushFont(nullptr,font_size*.65f);
        ImGui::TextDisabled("UMBRELLA CHRONICLES");ImGui::PopFont();
        ImGui::PushFont(nullptr,font_size*1.3f);
        ImGui::TextWrapped("%s",tr("update_title"));ImGui::PopFont();
        ImGui::Spacing();
        if(!controller.release()) {
            ImGui::TextWrapped("%s",tr("update_error"));
            ImGui::TextWrapped("%s",controller.error()->detail.c_str());
            if(ImGui::TextLink("GitHub Releases")) {
                if(!SDL_OpenURL("https://github.com/solleo95/reuc-recomp/releases/latest"))
                    std::fprintf(stderr,"[update] Cannot open release page: %s\n",SDL_GetError());
            }
            ImGui::Spacing();
            if(ImGui::Button(tr("update_later"))) {controller.postpone();ImGui::CloseCurrentPopup();}
            ImGui::EndPopup();ImGui::PopStyleVar();return false;
        }
        const auto& release=*controller.release();
        ImGui::TextDisabled("%s: %s",tr("update_current"),update::format_version(controller.installed_version()).c_str());
        ImGui::Text("%s: %s",tr("update_new"),update::format_version(release.version).c_str());
        ImGui::SameLine();ImGui::TextDisabled(" / %.1f MiB",release.size/(1024.*1024.));
        ImGui::Spacing();
        if(!release.notes.empty()) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg,ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{16*scale,14*scale});
            const auto wrapped=ImGui::CalcTextSize(release.notes.data(),release.notes.data()+release.notes.size(),false,
                ImGui::GetContentRegionAvail().x-48*scale);
            const float height=std::min(wrapped.y+32*scale,std::min(180*scale,display.y*.22f));
            ImGui::BeginChild("release-notes",{0,height},ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PushTextWrapPos(0);ImGui::TextUnformatted(release.notes.data(),release.notes.data()+release.notes.size());ImGui::PopTextWrapPos();
            ImGui::EndChild();
            ImGui::PopStyleVar();ImGui::PopStyleColor();ImGui::Spacing();
        }
        ImGui::PushStyleColor(ImGuiCol_Text,ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s",tr("update_restart"));ImGui::PopStyleColor();ImGui::Spacing();
        if(state==UpdateState::downloading || state==UpdateState::verifying || controller.close_requested()) {
            ImGui::TextWrapped("%s",tr(state==UpdateState::downloading?"update_downloading":"update_verifying"));
            ImGui::ProgressBar(release.size?static_cast<float>(double(controller.downloaded())/release.size):0.f,{-1,0});
            if(ImGui::Button(tr("update_cancel"))) controller.cancel();
        } else {
            if(game_running || (controller.error() && controller.error()->code==update::ErrorCode::busy))
                ImGui::TextWrapped("%s",tr("update_close_game"));
            else if(controller.error()) {
                ImGui::TextWrapped("%s",tr("update_error"));
                ImGui::TextWrapped("%s",controller.error()->detail.c_str());
            }
            ImGui::BeginDisabled(game_running || controller.busy());
            ImGui::PushStyleColor(ImGuiCol_Button,{.616f,.082f,.133f,1});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,{.702f,.137f,.192f,1});
            if(ImGui::Button(tr(state==UpdateState::failed?"update_retry":"update_action"))) {controller.accept();accepted=true;}
            ImGui::PopStyleColor(2);
            ImGui::EndDisabled();ImGui::SameLine();
            if(ImGui::Button(tr("update_later"))) {controller.postpone();ImGui::CloseCurrentPopup();}
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();return accepted;
}
}
