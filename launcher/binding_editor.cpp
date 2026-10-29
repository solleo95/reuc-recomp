#include "binding_editor.h"
#include <algorithm>
#include <stdexcept>
namespace uc {
namespace {
size_t index(Action action) {
    const auto i=size_t(action);
    if(i>=size_t(Action::count))throw std::out_of_range("invalid action");
    return i;
}
bool modifier(PhysicalInput input) {return input.kind==PhysicalKind::key && input.code>=224 && input.code<=231;}
BindingTable table_of(PhysicalInput input) {
    switch(input.kind) {
    case PhysicalKind::pad_button:
    case PhysicalKind::pad_axis: return BindingTable::gamepad;
    case PhysicalKind::wii_button: return BindingTable::wiimote;
    default: return BindingTable::keyboard;
    }
}
}
BindingEditor::BindingEditor(Bindings bindings,BindingTable table):table_(table),bindings_(std::move(bindings)) {
    if(auto error=validate(bindings_))throw std::invalid_argument(*error);
}
std::optional<std::string> BindingEditor::validate(const Bindings& bindings) const {
    switch(table_) {
    case BindingTable::gamepad: return validate_pad_bindings(bindings);
    case BindingTable::wiimote: return validate_wii_bindings(bindings);
    default: return validate_bindings(bindings);
    }
}
std::optional<std::string> BindingEditor::add(Action action,const Binding& binding) {
    auto replacement=bindings_; replacement[index(action)].push_back(binding);
    error_=validate(replacement);
    if(!error_)bindings_=std::move(replacement);
    return error_;
}
void BindingEditor::remove(Action action,size_t entry) {
    auto& entries=bindings_[index(action)];
    if(entry>=entries.size())throw std::out_of_range("invalid binding index");
    entries.erase(entries.begin()+entry);
}
void BindingEditor::clear(Action action) {
    bindings_[index(action)].clear(); if(capture_==action)cancel_capture();
}
void BindingEditor::restore_defaults() {
    bindings_=table_==BindingTable::gamepad ? default_pad_bindings() : table_==BindingTable::wiimote ? default_wii_bindings() : default_bindings();
    cancel_capture();
}
void BindingEditor::begin_capture(Action action,bool replace) {index(action);cancel_capture();capture_=action;replace_=replace;}
void BindingEditor::cancel_capture() {capture_.reset();replace_=false;modifiers_.clear();error_.reset();}
void BindingEditor::capture(PhysicalInput input) {
    if(input.kind==PhysicalKind::key && table_==BindingTable::keyboard) {   // host keys of the port: the game never sees them (game/host_keys.h, the Tab speed key in uc_main)
        const auto& menu_keys=bindings_[size_t(Action::menu)];
        const bool menu_key=std::any_of(menu_keys.begin(),menu_keys.end(),[&](const Binding& b){return b.input==input;}) || (menu_keys.empty() && input.code==73);   // none bound: Insert
        if(input.code==19 || input.code==43) { error_="P (pause) and Tab (game speed) are reserved"; return; }
        if(*capture_==Action::menu && input.code==41) { error_="Escape closes the menu: it cannot open it"; return; }
        if(*capture_!=Action::menu && menu_key) { error_="This key opens the in-game menu"; return; }
        if(*capture_==Action::menu) for(size_t a=0;a<bindings_.size();++a) if(a!=size_t(Action::menu))
            for(const auto& b:bindings_[a]) if(b.kind==BindingKind::physical && b.input==input && b.modifiers.empty()) { error_="This key is bound to another action"; return; }   // it would never reach it
    }
    auto modifiers=modifiers_; std::erase(modifiers,input);
    auto replacement=bindings_;auto& entries=replacement[index(*capture_)];
    if(replace_)entries.clear();
    entries.push_back({BindingKind::physical,input,std::move(modifiers)});
    error_=validate(replacement);
    if(!error_) {bindings_=std::move(replacement);cancel_capture();}
}
void BindingEditor::event(PhysicalInput input,bool down) {
    if(!capture_ || table_of(input)!=table_)return;   // each table waits for its own device
    if(modifier(input)) {
        const auto held=std::find(modifiers_.begin(),modifiers_.end(),input);
        if(down && held==modifiers_.end())modifiers_.push_back(input);
        else if(!down && held!=modifiers_.end())capture(input);
    } else if(down)capture(input);
}
void BindingEditor::wheel(int steps) {
    if(capture_ && steps && table_==BindingTable::keyboard)capture({PhysicalKind::wheel,uint16_t(steps>0 ? 1 : 2)});
}
void BindingEditor::focus(bool focused) {if(!focused)cancel_capture();}
}
