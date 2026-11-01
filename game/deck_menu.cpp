#include "deck_menu.h"
#include "gamepad_panel.h"
#include "graphite.h"
#include "input_config.h"
#include "localization.h"
#include "panel_widgets.h"
#ifdef _WIN32
#include <intrin.h>   // Load Clang's intrinsic guards before SDL 3.2.0's compatibility definitions.
#endif
#include <SDL3/SDL.h>
#include "imgui.h"
#include <algorithm>
#include <cmath>
namespace uc {
namespace {
constexpr uint64_t kRepeatFirstNs=380000000ull, kRepeatNs=70000000ull;   // a held direction repeats after 0.38 s, then every 70 ms
constexpr uint32_t kAccent=0x9d1522, kAccentHot=0xb32331, kIvory=0xeee9dc, kMuted=0x929899, kPanel=0x141819, kSide=0x101415, kRow=0x1c2122, kRowHot=0x2b3234, kTrack=0x3b4244;
constexpr uint32_t kDefaultReticle[2]={0xFF3030,0x3264FF};   // what the game draws the first / second player's reticle with
ImU32 rgb(uint32_t value,float alpha=1) { return IM_COL32((value>>16)&255,(value>>8)&255,value&255,int(std::clamp(alpha,0.f,1.f)*255+.5f)); }
void text(ImDrawList& draw,float size,ImVec2 at,ImU32 color,const char* value) { draw.AddText(ImGui::GetFont(),size,at,color,value); }
float text_width(float size,const char* value) { return ImGui::GetFont()->CalcTextSizeA(size,1e9f,0,value).x; }
void right_text(ImDrawList& draw,float size,float right,float y,ImU32 color,const char* value) { text(draw,size,{std::round(right-text_width(size,value)),y},color,value); }
}
DeckMenu::DeckMenu(LiveSettings& live,std::string language,std::function<const FeatureReport*()> features,uint32_t monitor_width,uint32_t monitor_height)
    : live_(live),language_(std::move(language)),features_(std::move(features)),monitor_width_(monitor_width),monitor_height_(monitor_height) {}
const char* DeckMenu::tr(std::string_view key) const { return launcher::text(language_,key).data(); }
void DeckMenu::setup() { fonts_=ui::load_fonts(*ImGui::GetIO().Fonts); }
settings::Env DeckMenu::env() const {
    settings::Env result;
    result.monitor_width=monitor_width_; result.monitor_height=monitor_height_; result.vendor_upscalers=false;   // no DLSS or FSR on the Deck
    if(const auto* report=features_()) { result.features_known=true; result.effects=report->effects; result.anisotropy_max=report->anisotropy_max; }
    return result;
}
void DeckMenu::opened() {
    std::lock_guard lock(mutex_);
    nav_=DeckNav{}; queue_.clear(); has_hold_=false; stick_scroll_=0; close_requested_=quit_armed_=quit_requested_=false; button_sub_=0; moved_by_key_=true; list_was_open_=false; stick_dir_[0]=stick_dir_[1]=0;
    editor_=BindingEditor(current().gamepad.bindings,BindingTable::gamepad); was_mods_page_=false;
    right_pad_pressed_=click_open_=false; clicks_.clear();
}
void DeckMenu::menu_closed() { hold_capturing_=false; editor_.cancel_capture(); editor_.focus(false); capture_took_=false; right_pad_pressed_=false; if(click_open_) { std::lock_guard lock(mutex_); clicks_.push_back(false); click_open_=false; } }
std::vector<settings::Page> DeckMenu::pages() const {
    auto result=settings::pages_for(settings::deck_menu);
    if(!mods_) result.erase(std::remove(result.begin(),result.end(),settings::Page::mods),result.end());
    return result;
}
bool DeckMenu::custom_press(std::string_view id,uint64_t now_ns) {
    if(id=="gyro_hold_button") { hold_capturing_=true; capture_since_ns_=now_ns; return true; }   // press the button that holds the gyro on
    if(id=="gyro_recenter") { editor_.begin_capture(Action::recenter,true); capture_since_ns_=now_ns; return true; }   // the next pad button replaces the recenter binding
    if(id=="quit_game") { if(quit_armed_) quit_requested_=true; else quit_armed_=true; return true; }
    return false;
}
void DeckMenu::press(NavKey key) { std::lock_guard lock(mutex_); queue_.push_back(key); }
void DeckMenu::hold(NavKey key,bool down) {
    std::lock_guard lock(mutex_);
    if(down) { queue_.push_back(key); has_hold_=true; hold_key_=key; hold_next_ns_=SDL_GetTicksNS()+kRepeatFirstNs; }
    else if(has_hold_ && hold_key_==key) has_hold_=false;
}
bool DeckMenu::event(const SDL_Event& event) {
    const bool pad_event=event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type==SDL_EVENT_GAMEPAD_BUTTON_UP || event.type==SDL_EVENT_GAMEPAD_AXIS_MOTION;
    if(pad_event && (editor_.capturing() || hold_capturing_ || capture_took_)) { capture_took_=false; return true; }   // every pad button belongs to the capture, also the one that ended it
    switch(event.type) {
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP: {   // the right trackpad, pressed firmly: a click at the cursor
        if(event.gtouchpad.touchpad!=1 || event.gtouchpad.finger!=0) return false;
        const float pressure=event.type==SDL_EVENT_GAMEPAD_TOUCHPAD_UP ? 0.f : event.gtouchpad.pressure;
        const bool was=right_pad_pressed_, now=was ? pressure>=kTrackpadPressOff : pressure>=kTrackpadPressOn;
        right_pad_pressed_=now;
        if(now==was) return false;
        std::lock_guard lock(mutex_);
        if(now) { if(!editor_.capturing() && !capture_took_) { clicks_.push_back(true); click_open_=true; } }   // a press that binds a button is not also a click
        else if(click_open_) { clicks_.push_back(false); click_open_=false; }
        return false;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down=event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        switch(event.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: hold(NavKey::up,down); return true;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: hold(NavKey::down,down); return true;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: hold(NavKey::left,down); return true;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: hold(NavKey::right,down); return true;
        case SDL_GAMEPAD_BUTTON_SOUTH: if(down) press(NavKey::accept); return true;
        case SDL_GAMEPAD_BUTTON_EAST: if(down) press(NavKey::back); return true;
        case SDL_GAMEPAD_BUTTON_WEST: if(down) press(NavKey::add); return true;
        case SDL_GAMEPAD_BUTTON_NORTH: if(down) press(NavKey::clear); return true;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: if(down) press(NavKey::page_prev); return true;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: if(down) press(NavKey::page_next); return true;
        default: return false;
        }
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const float value=float(event.gaxis.value)/32767.f;
        const auto direction=[&](int axis,NavKey negative,NavKey positive) {   // the left stick is a D-pad with a dead zone and some hysteresis
            const int old=stick_dir_[axis]; const int now=value>(old>0 ? .35f : .6f) ? 1 : value<-(old<0 ? .35f : .6f) ? -1 : 0;
            if(now==old) return;
            if(old) hold(old<0 ? negative : positive,false);
            if(now) hold(now<0 ? negative : positive,true);
            stick_dir_[axis]=now;
        };
        if(event.gaxis.axis==SDL_GAMEPAD_AXIS_LEFTX) { direction(0,NavKey::left,NavKey::right); return true; }
        if(event.gaxis.axis==SDL_GAMEPAD_AXIS_LEFTY) { direction(1,NavKey::up,NavKey::down); return true; }
        if(event.gaxis.axis==SDL_GAMEPAD_AXIS_RIGHTY) { std::lock_guard lock(mutex_); stick_scroll_=std::abs(value)>.2f ? value : 0; return true; }
        return false;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        if(event.key.repeat) return false;
        const bool down=event.type==SDL_EVENT_KEY_DOWN;
        switch(event.key.scancode) {   // a keyboard stands in for the pad (a desktop test run, a dock)
        case SDL_SCANCODE_UP: hold(NavKey::up,down); return true;
        case SDL_SCANCODE_DOWN: hold(NavKey::down,down); return true;
        case SDL_SCANCODE_LEFT: hold(NavKey::left,down); return true;
        case SDL_SCANCODE_RIGHT: hold(NavKey::right,down); return true;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: if(down) press(NavKey::accept); return true;
        case SDL_SCANCODE_BACKSPACE: if(down) press(NavKey::back); return true;
        case SDL_SCANCODE_PAGEUP: if(down) press(NavKey::page_prev); return true;
        case SDL_SCANCODE_PAGEDOWN: if(down) press(NavKey::page_next); return true;
        default: return false;
        }
    }
    default: return false;
    }
}
void DeckMenu::commit() {
    if(pending_) { const auto next=*pending_; pending_.reset(); live_.apply(next); }
    if(close_requested_) { close_requested_=false; if(close_) close_(); }
    if(quit_requested_) { quit_requested_=false; if(quit_) quit_(); }
}
void DeckMenu::build() {
    auto& io=ImGui::GetIO();
    const float s=std::max(.6f,io.DisplaySize.y/720.f);   // 1 at 1280x720
    const auto pages=this->pages();
    const int page_count=int(pages.size());
    auto options=current(); const auto where=env();
    bool changed=false;
    const auto now_ns=SDL_GetTicksNS();
    capture_took_=false;   // the pad event it was about has been dispatched long ago (event handling and rendering share the main loop)
    if(editor_.capturing() && now_ns-capture_since_ns_>8000000000ull) editor_.cancel_capture();   // nobody pressed a button: the capture ends
    if(hold_capturing_ && now_ns-capture_since_ns_>8000000000ull) hold_capturing_=false;
    {   // the answer of the gyro's hold-button row
        std::lock_guard lock(mutex_);
        if(hold_result_) { options.gamepad.gyro_hold=*hold_result_; changed=true; hold_result_.reset(); }
    }
    nav_.page=std::clamp(nav_.page,0,std::max(0,page_count-1));
    const auto page_of=[&] { return pages[size_t(nav_.page)]; };
    if(page_of()==settings::Page::mods) io.ConfigFlags|=ImGuiConfigFlags_NavEnableGamepad; else io.ConfigFlags&=~ImGuiConfigFlags_NavEnableGamepad;   // the mod manager's own widgets: ImGui moves the focus with the pad there
    std::deque<NavKey> keys; std::deque<bool> clicks; float scroll=0;
    {   // the keys of this frame: the queue and the repeat of a direction that is held
        std::lock_guard lock(mutex_);
        keys.swap(queue_); clicks.swap(clicks_); scroll=stick_scroll_;
        const auto now=SDL_GetTicksNS();
        if(has_hold_ && now>=hold_next_ns_) { keys.push_back(hold_key_); hold_next_ns_=now+kRepeatNs; }
    }
    for(const bool down:clicks) io.AddMouseButtonEvent(ImGuiMouseButton_Left,down);   // the right trackpad's press: ImGui takes it from the next frame, at the cursor
    const auto actions=gamepad_panel_actions();
    const int button_rows=int(actions.size())+1;   // every action of the gamepad table, then "restore defaults"
    for(const auto key:keys) {
        const auto page=page_of();
        if(key!=NavKey::accept || page!=settings::Page::quit) quit_armed_=false;   // the second press must follow the first at once
        if(hold_capturing_) { if(key==NavKey::back) hold_capturing_=false; continue; }   // a keyboard can give up; the pad's buttons belong to the capture (event())
        if(page!=settings::Page::buttons && editor_.capturing()) { if(key==NavKey::back) editor_.cancel_capture(); continue; }   // the same for the recenter row (the Buttons page handles its own)
        if(key==NavKey::up || key==NavKey::down || key==NavKey::page_prev || key==NavKey::page_next) { moved_by_key_=true; moved_list_=true; button_sub_=0; }   // a row that was just reached starts on the row itself
        if(page==settings::Page::buttons && key!=NavKey::page_prev && key!=NavKey::page_next) {   // the gamepad's buttons: a row per action
            nav_.row=std::clamp(nav_.row,0,button_rows-1);
            const bool on_action=nav_.row<int(actions.size());
            const Action action=on_action ? actions[size_t(nav_.row)] : Action::interact;
            switch(key) {
            case NavKey::up: nav_.row=std::max(0,nav_.row-1); break;
            case NavKey::down: nav_.row=std::min(button_rows-1,nav_.row+1); break;
            case NavKey::left: if(on_action) button_sub_=std::max(0,button_sub_-1); break;    // the row itself, its plus, its cross
            case NavKey::right: if(on_action) button_sub_=std::min(2,button_sub_+1); break;
            case NavKey::accept:   // on the row: replace the bindings; on the plus: add one; on the cross: clear them
                if(!on_action) editor_.restore_defaults();
                else if(button_sub_==2) editor_.clear(action);
                else { editor_.begin_capture(action,button_sub_==0); capture_since_ns_=now_ns; }
                break;
            case NavKey::add: if(on_action) { editor_.begin_capture(action); capture_since_ns_=now_ns; } break;
            case NavKey::clear: if(on_action) editor_.clear(action); break;
            case NavKey::back: if(editor_.capturing()) editor_.cancel_capture(); else close_requested_=true; break;
            default: break;
            }
            continue;
        }
        if(page==settings::Page::mods && key==NavKey::back && ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel)) continue;   // a mod's window closes first (ImGui takes the button itself)
        const auto rows=deck_rows(page,options,where);
        if(key==NavKey::accept && !rows.empty() && custom_press(rows[size_t(std::clamp(nav_.row,0,int(rows.size())-1))]->id,now_ns)) continue;
        const auto result=nav_.handle(key,options,where,rows,page_count);
        changed|=result.changed; if(result.close) close_requested_=true;
    }
    if((page_of()==settings::Page::buttons || page_of()==settings::Page::gamepad) && !editor_.capturing() && editor_.bindings()!=options.gamepad.bindings) { options.gamepad.bindings=editor_.bindings(); changed=true; }   // the table follows its editor
    auto rows=deck_rows(page_of(),options,where);
    {
        const int row_total=page_of()==settings::Page::buttons ? button_rows : page_of()==settings::Page::mods ? 1 : int(rows.size());
        nav_.keep_inside(row_total,page_count);
    }
    const settings::Text translate=[this](std::string_view key) { return std::string(tr(key)); };

    launcher::graphite_style();
    ImGui::GetStyle().ScrollbarSize=8*s;
    // dim the game behind the panel
    ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,rgb(0x000000,.62f));
    ImGui::Begin("##deck_backdrop",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::End(); ImGui::PopStyleColor();

    const ImVec2 panel{std::min(io.DisplaySize.x-32*s,1180*s),std::min(io.DisplaySize.y-32*s,660*s)};
    ImGui::SetNextWindowPos({io.DisplaySize.x/2,io.DisplaySize.y/2},ImGuiCond_Always,{.5f,.5f}); ImGui::SetNextWindowSize(panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,16*s); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,1);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,rgb(kPanel,.98f)); ImGui::PushStyleColor(ImGuiCol_Border,rgb(kTrack));
    ImGui::PushFont(fonts_.sans,24*s);
    ImGui::Begin("##deck_menu",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoNav);
    const bool panel_focused=ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    auto& draw=*ImGui::GetWindowDrawList();
    const ImVec2 origin=ImGui::GetWindowPos();
    const float side=272*s, footer=70*s, pad=30*s;

    // sidebar: the title and the pages
    draw.AddRectFilled(origin,{origin.x+side,origin.y+panel.y},rgb(kSide),16*s,ImDrawFlags_RoundCornersLeft);
    ImGui::PushFont(fonts_.serif,34*s); text(draw,34*s,{origin.x+26*s,origin.y+24*s},rgb(kIvory),tr("deck_menu_title")); ImGui::PopFont();
    for(int i=0;i<page_count;++i) {
        const ImVec2 at{origin.x+14*s,origin.y+(100+i*66)*s}, size{side-28*s,58*s};
        ImGui::SetCursorScreenPos(at); ImGui::PushID(i);
        ImGui::InvisibleButton("##page",size);
        if(ImGui::IsItemClicked()) { nav_.page=i; nav_.row=0; nav_.list_open=false; moved_by_key_=true; quit_armed_=false; button_sub_=0; }
        const bool selected=i==nav_.page, hot=ImGui::IsItemHovered();
        draw.AddRectFilled(at,{at.x+size.x,at.y+size.y},selected ? rgb(kAccent) : hot ? rgb(kRowHot) : rgb(kSide),10*s);
        text(draw,25*s,{at.x+20*s,std::round(at.y+(size.y-25*s)/2)},rgb(selected ? 0xffffff : kIvory),tr(settings::page_label_key(pages[size_t(i)])));
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos({origin.x+side+pad,origin.y+panel.y-footer}); ImGui::Dummy({1,1});

    // the page title and a close button
    const float left=origin.x+side+pad, right=origin.x+panel.x-pad;
    ImGui::PushFont(fonts_.serif,32*s); text(draw,32*s,{left,origin.y+24*s},rgb(kIvory),tr(settings::page_label_key(pages[size_t(nav_.page)]))); ImGui::PopFont();
    {
        const ImVec2 at{right-48*s,origin.y+18*s}, size{48*s,48*s};
        ImGui::SetCursorScreenPos(at); ImGui::InvisibleButton("##close",size);
        if(ImGui::IsItemClicked()) close_requested_=true;
        draw.AddRectFilled(at,{at.x+size.x,at.y+size.y},ImGui::IsItemHovered() ? rgb(kRowHot) : rgb(kRow),24*s);
        const ImVec2 c{at.x+size.x/2,at.y+size.y/2}; const float k=9*s;
        draw.AddLine({c.x-k,c.y-k},{c.x+k,c.y+k},rgb(kIvory),2.5f*s); draw.AddLine({c.x-k,c.y+k},{c.x+k,c.y-k},rgb(kIvory),2.5f*s);
    }

    // the rows
    const float rows_top=origin.y+84*s, rows_height=panel.y-84*s-footer;
    const auto current_page=page_of();
    const PanelText panel_text=[this](std::string_view key) { return tr(key); };
    if(current_page!=settings::Page::mods) {   // the Mods page is a window of its own, further down
    ImGui::SetCursorScreenPos({left-10*s,rows_top});
    ImGui::PushStyleColor(ImGuiCol_ChildBg,rgb(0x000000,0));
    ImGui::BeginChild("##rows",{right-left+20*s,rows_height},ImGuiChildFlags_None,ImGuiWindowFlags_NoNav);
    ImGui::PopStyleColor();
    if(scroll!=0) ImGui::SetScrollY(ImGui::GetScrollY()+scroll*1100*s*io.DeltaTime);   // the right stick
    if(current_page==settings::Page::buttons) {   // one row per action of the gamepad table: its buttons as pictures, a button to add one and one to clear them
        auto& rows_draw=*ImGui::GetWindowDrawList();
        const float width=ImGui::GetContentRegionAvail().x-10*s, row_height=64*s, gap=8*s, small=46*s;
        ImGui::Dummy({0,4*s});
        for(int i=0;i<button_rows;++i) {
            const bool defaults=i==int(actions.size()), focused=i==nav_.row;
            const bool waiting=!defaults && editor_.capturing()==actions[size_t(i)];
            const ImVec2 at=ImGui::GetCursorScreenPos(), end{at.x+width,at.y+row_height};
            const float tools=defaults ? 0 : 2*small+22*s;
            ImGui::PushID(i);
            ImGui::InvisibleButton("##row",{width-tools,row_height});
            const bool hovered=ImGui::IsItemHovered();
            if(ImGui::IsItemActivated()) { press_x_=io.MousePos.x; press_y_=io.MousePos.y; dragged_=false; }
            if(ImGui::IsItemActive() && std::abs(io.MousePos.y-press_y_)>10*s) { dragged_=true; ImGui::SetScrollY(ImGui::GetScrollY()-io.MouseDelta.y); }
            if(ImGui::IsItemDeactivated() && !dragged_ && hovered) {   // a tap replaces the bindings (the capture waits for a button), the last row restores the defaults
                nav_.row=i;
                if(defaults) editor_.restore_defaults(); else { editor_.begin_capture(actions[size_t(i)],true); capture_since_ns_=now_ns; }
            }
            if(focused && moved_by_key_) ImGui::SetScrollHereY(.5f);
            if(!defaults) {
                const float top=at.y+(row_height-small)/2;
                ImGui::SetCursorScreenPos({end.x-2*small-14*s,top}); ImGui::InvisibleButton("##add",{small,small});
                const bool add_hot=ImGui::IsItemHovered();
                if(ImGui::IsItemClicked()) { nav_.row=i; editor_.begin_capture(actions[size_t(i)]); capture_since_ns_=now_ns; }
                const ImVec2 add_at=ImGui::GetItemRectMin();
                ImGui::SetCursorScreenPos({end.x-small-8*s,top}); ImGui::InvisibleButton("##clear",{small,small});
                const bool clear_hot=ImGui::IsItemHovered();
                if(ImGui::IsItemClicked()) { nav_.row=i; editor_.clear(actions[size_t(i)]); }
                const ImVec2 clear_at=ImGui::GetItemRectMin();
                ImGui::SetCursorScreenPos({at.x,at.y+row_height});
                rows_draw.AddRectFilled(at,end,focused ? rgb(kAccent,.42f) : hovered ? rgb(kRowHot) : rgb(kRow,.92f),10*s);
                if(focused) rows_draw.AddRect(at,end,rgb(kAccentHot),10*s,0,2*s);
                text(rows_draw,25*s,{at.x+22*s,std::round(at.y+(row_height-25*s)/2)},rgb(kIvory),tr(action_names[size_t(actions[size_t(i)])]));
                const ImVec2 low{at.x+width*.33f,at.y+7*s}, high{end.x-tools-6*s,end.y-7*s};
                if(waiting) {
                    const char* hint=tr("deck_press_button");
                    right_text(rows_draw,24*s,(low.x+high.x+text_width(24*s,hint))/2,std::round((low.y+high.y)/2-12*s),rgb(kIvory),hint);
                } else panel_shared::draw_pieces(rows_draw,low,high,panel_shared::pieces(editor_.bindings()[size_t(actions[size_t(i)])],panel_text),PadFamily::steam_deck,tr("unbound"));
                const float k=8*s;   // the add button's plus and the clear button's cross
                const bool add_selected=focused && button_sub_==1, clear_selected=focused && button_sub_==2;   // the D-pad's left and right move onto them
                rows_draw.AddRectFilled(add_at,{add_at.x+small,add_at.y+small},add_selected ? rgb(kAccent) : add_hot ? rgb(kTrack) : rgb(kSide),small/2);
                if(add_selected) rows_draw.AddCircle({add_at.x+small/2,add_at.y+small/2},small/2+3*s,rgb(kAccentHot),32,2.5f*s);
                const ImVec2 plus{add_at.x+small/2,add_at.y+small/2};
                rows_draw.AddLine({plus.x-k,plus.y},{plus.x+k,plus.y},rgb(kIvory),2.5f*s); rows_draw.AddLine({plus.x,plus.y-k},{plus.x,plus.y+k},rgb(kIvory),2.5f*s);
                rows_draw.AddRectFilled(clear_at,{clear_at.x+small,clear_at.y+small},clear_selected ? rgb(kAccent) : clear_hot ? rgb(kTrack) : rgb(kSide),small/2);
                if(clear_selected) rows_draw.AddCircle({clear_at.x+small/2,clear_at.y+small/2},small/2+3*s,rgb(kAccentHot),32,2.5f*s);
                const ImVec2 cross{clear_at.x+small/2,clear_at.y+small/2}, c=cross; const float m=7*s;
                rows_draw.AddLine({c.x-m,c.y-m},{c.x+m,c.y+m},rgb(kIvory),2.5f*s); rows_draw.AddLine({c.x-m,c.y+m},{c.x+m,c.y-m},rgb(kIvory),2.5f*s);
            } else {
                rows_draw.AddRectFilled(at,end,focused ? rgb(kAccent,.42f) : hovered ? rgb(kRowHot) : rgb(kRow,.92f),10*s);
                if(focused) rows_draw.AddRect(at,end,rgb(kAccentHot),10*s,0,2*s);
                const char* label=tr("defaults");
                text(rows_draw,25*s,{std::round(at.x+(width-text_width(25*s,label))/2),std::round(at.y+(row_height-25*s)/2)},rgb(kIvory),label);
            }
            ImGui::PopID();
            ImGui::SetCursorScreenPos({at.x,at.y+row_height});
            ImGui::Dummy({width,gap});
        }
        ImGui::Dummy({0,8*s});
    } else {
        auto& rows_draw=*ImGui::GetWindowDrawList();
        const float width=ImGui::GetContentRegionAvail().x-10*s, row_height=64*s, gap=8*s;
        ImGui::Dummy({0,4*s});
        for(size_t i=0;i<rows.size();++i) {
            const auto& item=*rows[i];
            const settings::State state=item.state ? item.state(options,where) : settings::State{};
            const bool slider=item.kind==settings::Kind::slider, focused=int(i)==nav_.row;
            const float bar_width=slider ? 250*s : 0, value_width=slider ? 100*s : 0;
            const ImVec2 at=ImGui::GetCursorScreenPos(), end{at.x+width,at.y+row_height};
            ImGui::PushID(int(i));
            ImGui::InvisibleButton("##row",{width-(slider ? bar_width+value_width+44*s : 0),row_height});
            const bool hovered=ImGui::IsItemHovered();
            if(ImGui::IsItemActivated()) { press_x_=io.MousePos.x; press_y_=io.MousePos.y; dragged_=false; }
            if(ImGui::IsItemActive() && std::abs(io.MousePos.y-press_y_)>10*s) { dragged_=true; ImGui::SetScrollY(ImGui::GetScrollY()-io.MouseDelta.y); }   // a finger drags the list
            if(ImGui::IsItemDeactivated() && !dragged_ && hovered) {   // a tap: one tap does it
                nav_.row=int(i);
                if(!custom_press(item.id,now_ns)) {
                    const auto result=nav_.handle(NavKey::accept,options,where,rows,page_count);
                    changed|=result.changed;
                }
            }
            if(focused && moved_by_key_) ImGui::SetScrollHereY(.5f);
            float bar_x=0;
            if(slider) {   // the bar and its value sit at the right of the row; dragging on the bar sets the value
                bar_x=end.x-value_width-bar_width-30*s;
                ImGui::SetCursorScreenPos({bar_x-14*s,at.y});
                ImGui::InvisibleButton("##bar",{bar_width+28*s,row_height});
                if(ImGui::IsItemActive() && state.enabled) {
                    const float t=std::clamp((io.MousePos.x-bar_x)/bar_width,0.f,1.f);
                    nav_.row=int(i);
                    changed|=settings::set_slider(item,options,item.min+int(std::lround(t*(item.max-item.min))));
                }
                ImGui::SetCursorScreenPos({at.x,at.y+row_height});
            }
            const bool lit=hovered || ImGui::IsItemActive();
            rows_draw.AddRectFilled(at,end,focused ? rgb(kAccent,.42f) : lit ? rgb(kRowHot) : rgb(kRow,.92f),10*s);
            if(focused) rows_draw.AddRect(at,end,rgb(kAccentHot),10*s,0,2*s);
            const ImU32 ink=rgb(state.enabled ? kIvory : kMuted,state.enabled ? 1.f : .75f);
            const float label_y=state.enabled || !state.why ? at.y+(row_height-25*s)/2 : at.y+9*s;
            const bool ask_quit=quit_armed_ && std::string_view(item.id)=="quit_game";   // the row itself asks for the second press
            text(rows_draw,25*s,{at.x+22*s,std::round(label_y)},ask_quit ? rgb(kAccentHot) : ink,tr(ask_quit ? "quit_game_confirm" : item.label_key));
            if(!state.enabled && state.why) text(rows_draw,18*s,{at.x+22*s,at.y+row_height-27*s},rgb(kMuted),tr(state.why));
            const float edge=end.x-22*s, mid=at.y+row_height/2;
            const float fade=state.enabled ? 1.f : .5f;
            switch(item.kind) {
            case settings::Kind::toggle: {   // a switch
                const bool on=item.get_bool(options); const ImVec2 a{edge-68*s,mid-17*s}, b{edge,mid+17*s};
                rows_draw.AddRectFilled(a,b,on ? rgb(kAccent,fade) : rgb(kTrack,fade),17*s);
                rows_draw.AddCircleFilled({on ? b.x-17*s : a.x+17*s,mid},13*s,rgb(kIvory,fade),24);
                break;
            }
            case settings::Kind::choice: {   // the value and a chevron: a tap opens the list
                const auto labels=item.labels(options,where,translate); const int index=item.index(options,where);
                const std::string shown=index>=0 && index<int(labels.size()) ? labels[size_t(index)] : "—";
                right_text(rows_draw,24*s,edge-34*s,std::round(mid-12*s),ink,shown.c_str());
                const float x=edge-12*s;
                rows_draw.AddLine({x-7*s,mid-4*s},{x,mid+4*s},ink,2*s); rows_draw.AddLine({x,mid+4*s},{x+7*s,mid-4*s},ink,2*s);
                break;
            }
            case settings::Kind::slider: {
                const float t=item.max>item.min ? float(item.get_int(options)-item.min)/float(item.max-item.min) : 0;
                rows_draw.AddRectFilled({bar_x,mid-4*s},{bar_x+bar_width,mid+4*s},rgb(kTrack,fade),4*s);
                rows_draw.AddRectFilled({bar_x,mid-4*s},{bar_x+bar_width*t,mid+4*s},rgb(kAccentHot,fade),4*s);
                rows_draw.AddCircleFilled({bar_x+bar_width*t,mid},14*s,rgb(kIvory,fade),24);
                const auto value=settings::slider_text(item,options);
                right_text(rows_draw,24*s,edge,std::round(mid-12*s),ink,value.c_str());
                break;
            }
            case settings::Kind::color: {
                const uint32_t value=item.get_color(options);
                const uint32_t shown=value ? value : kDefaultReticle[std::string(item.id).find("p2")!=std::string::npos ? 1 : 0];
                const ImVec2 a{edge-56*s,mid-15*s}, b{edge,mid+15*s};
                rows_draw.AddRectFilled(a,b,rgb(shown),7*s); rows_draw.AddRect(a,b,rgb(kIvory,.6f),7*s,0,1.5f*s);
                if(!value) right_text(rows_draw,22*s,a.x-14*s,std::round(mid-11*s),rgb(kMuted),tr("reticle_default"));
                break;
            }
            case settings::Kind::custom:
                if(std::string_view(item.id)=="gyro_recenter") {   // the recenter binding: its picture, or the prompt while the row waits for a press
                    const ImVec2 low{edge-300*s,at.y+7*s}, high{edge,end.y-7*s};
                    if(editor_.capturing()==Action::recenter) { const char* hint=tr("deck_press_button"); right_text(rows_draw,24*s,edge,std::round(mid-12*s),rgb(kIvory),hint); }
                    else panel_shared::draw_pieces(rows_draw,low,high,panel_shared::pieces(editor_.bindings()[size_t(Action::recenter)],panel_text),PadFamily::steam_deck,tr("unbound"));
                }
                if(std::string_view(item.id)=="gyro_hold_button") {   // the button that holds the gyro on: its picture, or the prompt while the row waits for a press
                    const ImVec2 low{edge-300*s,at.y+7*s}, high{edge,end.y-7*s};
                    if(hold_capturing_) { const char* hint=tr("deck_press_button"); right_text(rows_draw,24*s,edge,std::round(mid-12*s),rgb(kIvory),hint); }
                    else panel_shared::draw_pieces(rows_draw,low,high,{panel_shared::Piece{{},options.gamepad.gyro_hold}},PadFamily::steam_deck,tr("unbound"));
                }
                break;
            }
            ImGui::PopID();
            ImGui::SetCursorScreenPos({at.x,at.y+row_height});
            ImGui::Dummy({width,gap});
        }
        ImGui::Dummy({0,8*s});
    }
    moved_by_key_=false;
    ImGui::EndChild();
    }

    // the footer: what the buttons do
    {
        const float y=origin.y+panel.y-footer+16*s, size=34*s;
        float x=left;
        const auto pill=[&](const char* label,ImU32 fill=rgb(kTrack)) {   // a rounded label for a shoulder button or View
            const float w=text_width(20*s,label)+22*s; const ImVec2 a{x,y},b{x+w,y+size};
            draw.AddRectFilled(a,b,fill,size/2); text(draw,20*s,{std::round(x+11*s),std::round(y+(size-20*s)/2)},rgb(kIvory),label); x+=w+8*s;
        };
        const auto face=[&](const char* letter) {   // A and B as the Deck's face buttons: plain white letters, no colours
            const ImVec2 c{x+size/2,y+size/2};
            draw.AddCircleFilled(c,size/2,rgb(0x181b1c),28); draw.AddCircle(c,size/2-1,rgb(kMuted),28,1.5f*s);
            text(draw,22*s,{std::round(x+size/2-text_width(22*s,letter)/2),std::round(y+(size-22*s)/2)},rgb(0xffffff),letter); x+=size+8*s;
        };
        const auto label=[&](const char* value) { text(draw,22*s,{x,std::round(y+(size-22*s)/2)},rgb(kMuted),value); x+=text_width(22*s,value)+30*s; };
        const auto dpad=[&] {   // left and right of the D-pad: a pill with two arrows
            const float w=size*1.7f; const ImVec2 c{x+w/2,y+size/2};
            draw.AddRectFilled({x,y},{x+w,y+size},rgb(kTrack),size/2);
            draw.AddTriangleFilled({c.x-w*.30f,c.y},{c.x-w*.12f,c.y-7*s},{c.x-w*.12f,c.y+7*s},rgb(kIvory));
            draw.AddTriangleFilled({c.x+w*.30f,c.y},{c.x+w*.12f,c.y-7*s},{c.x+w*.12f,c.y+7*s},rgb(kIvory));
            x+=w+8*s;
        };
        const bool buttons_page=current_page==settings::Page::buttons;
        if(editor_.capturing()) label(tr("deck_press_button"));   // the pad's buttons belong to the capture
        else face("A"), label(tr(buttons_page ? "deck_hint_replace" : "deck_hint_select"));
        if(editor_.capturing()) {}
        else if(!nav_.list_open) {
            if(buttons_page) { face("X"); label(tr("deck_hint_add")); face("Y"); label(tr("deck_hint_clear")); }
            else if(current_page!=settings::Page::mods) { dpad(); label(tr("deck_hint_change")); }
            face("B"); label(tr("deck_hint_back"));
            pill("L1"); pill("R1"); label(tr("deck_hint_page"));
            pill("View"); label(tr("deck_hint_close"));
        } else { face("B"); label(tr("deck_hint_back")); }
        if(const auto& error=live_.error()) {
            ImGui::SetCursorScreenPos({left,y-30*s}); ImGui::PushStyleColor(ImGuiCol_Text,ImVec4{1,.52f,.52f,1});
            ImGui::PushFont(fonts_.sans,18*s); ImGui::TextUnformatted(error->c_str()); ImGui::PopFont(); ImGui::PopStyleColor();
        }
    }

    // a choice's list
    if(nav_.list_open || ImGui::IsPopupOpen("##choice")) {
        const settings::Item* item=rows.empty() ? nullptr : rows[size_t(nav_.row)];
        if(nav_.list_open && !ImGui::IsPopupOpen("##choice")) { ImGui::OpenPopup("##choice"); list_was_open_=false; }
        const auto labels=item && item->kind==settings::Kind::choice ? item->labels(options,where,translate) : std::vector<std::string>{};
        const float item_height=60*s, list_width=std::min(580*s,io.DisplaySize.x-60*s), list_height=std::min(io.DisplaySize.y-80*s,float(labels.size())*item_height+24*s);
        ImGui::SetNextWindowPos({io.DisplaySize.x/2,io.DisplaySize.y/2},ImGuiCond_Always,{.5f,.5f}); ImGui::SetNextWindowSize({list_width,list_height});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{12*s,12*s}); ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,16*s);
        ImGui::PushStyleColor(ImGuiCol_PopupBg,rgb(kPanel)); ImGui::PushStyleColor(ImGuiCol_Border,rgb(kAccent));
        if(ImGui::BeginPopupModal("##choice",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoNav)) {
            if(!nav_.list_open || !item) ImGui::CloseCurrentPopup();
            else {
                auto& list_draw=*ImGui::GetWindowDrawList();
                const int current_index=item->index(options,where);
                for(size_t i=0;i<labels.size();++i) {
                    ImGui::PushID(int(i));
                    const ImVec2 at=ImGui::GetCursorScreenPos(), size{ImGui::GetContentRegionAvail().x,item_height-6*s};
                    ImGui::InvisibleButton("##entry",size);
                    if(ImGui::IsItemActivated()) { press_x_=io.MousePos.x; press_y_=io.MousePos.y; dragged_=false; }
                    if(ImGui::IsItemActive() && std::abs(io.MousePos.y-press_y_)>10*s) { dragged_=true; ImGui::SetScrollY(ImGui::GetScrollY()-io.MouseDelta.y); }
                    if(ImGui::IsItemDeactivated() && !dragged_ && ImGui::IsItemHovered()) {
                        nav_.list_index=int(i);
                        changed|=nav_.handle(NavKey::accept,options,where,rows,page_count).changed;
                    }
                    const bool picked=int(i)==nav_.list_index;
                    if(picked && (moved_list_ || !list_was_open_)) ImGui::SetScrollHereY(.5f);
                    list_draw.AddRectFilled(at,{at.x+size.x,at.y+size.y},picked ? rgb(kAccent) : ImGui::IsItemHovered() ? rgb(kRowHot) : rgb(kRow),10*s);
                    text(list_draw,25*s,{at.x+22*s,std::round(at.y+(size.y-25*s)/2)},rgb(0xffffff),labels[i].c_str());
                    if(int(i)==current_index) {   // the value in use
                        const ImVec2 c{at.x+size.x-30*s,at.y+size.y/2};
                        list_draw.AddLine({c.x-8*s,c.y},{c.x-2*s,c.y+6*s},rgb(kIvory),3*s); list_draw.AddLine({c.x-2*s,c.y+6*s},{c.x+9*s,c.y-7*s},rgb(kIvory),3*s);
                    }
                    ImGui::Dummy({0,6*s});
                    ImGui::PopID();
                }
                if(ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) nav_.list_open=false;   // a tap outside closes it
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2); ImGui::PopStyleVar(2);
        moved_list_=false;
        list_was_open_=nav_.list_open;
    } else list_was_open_=false;

    const bool focus_mods=current_page==settings::Page::mods && (!was_mods_page_ || (panel_focused && !ImGui::IsMouseDown(0)));   // the pad moves ImGui's focus inside the focused window only
    ImGui::End();
    ImGui::PopFont(); ImGui::PopStyleColor(2); ImGui::PopStyleVar(2);

    // the Mods page: the mod platform's own manager in a window over the content area, laid out in the same units as the desktop menu
    if(current_page==settings::Page::mods && mods_) {
        const auto saved=ImGui::GetStyle();
        ImGui::GetStyle().ScaleAllSizes(.82f*s);
        ImGui::SetNextWindowPos({origin.x+side+pad-10*s,origin.y+84*s}); ImGui::SetNextWindowSize({panel.x-side-2*pad+20*s,panel.y-84*s-footer});
        if(focus_mods) ImGui::SetNextWindowFocus();
        ImGui::PushStyleColor(ImGuiCol_WindowBg,rgb(0x000000,0)); ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{12*s,0});
        ImGui::PushFont(fonts_.sans,22*s);
        ImGui::Begin("##deck_mods",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove);
        mods_->manager_tab(.82f*s);
        ImGui::End();
        ImGui::PopFont(); ImGui::PopStyleVar(); ImGui::PopStyleColor();
        ImGui::GetStyle()=saved;
    }
    was_mods_page_=current_page==settings::Page::mods;
    if(changed) pending_=options;
}
}
