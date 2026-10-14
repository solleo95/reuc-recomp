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
}
BindingEditor::BindingEditor(Bindings bindings):bindings_(std::move(bindings)) {
    if(auto error=validate_bindings(bindings_))throw std::invalid_argument(*error);
}
std::optional<std::string> BindingEditor::add(Action action,const Binding& binding) {
    auto replacement=bindings_; replacement[index(action)].push_back(binding);
    error_=validate_bindings(replacement);
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
void BindingEditor::restore_defaults() {bindings_=default_bindings();cancel_capture();}
void BindingEditor::begin_capture(Action action) {index(action);cancel_capture();capture_=action;}
void BindingEditor::cancel_capture() {capture_.reset();modifiers_.clear();error_.reset();}
void BindingEditor::capture(PhysicalInput input) {
    auto modifiers=modifiers_; std::erase(modifiers,input);
    if(!add(*capture_,{BindingKind::physical,input,std::move(modifiers)}))cancel_capture();
}
void BindingEditor::event(PhysicalInput input,bool down) {
    if(!capture_)return;
    if(input==PhysicalInput{PhysicalKind::key,41} && down) {cancel_capture();return;}
    if(modifier(input)) {
        const auto held=std::find(modifiers_.begin(),modifiers_.end(),input);
        if(down && held==modifiers_.end())modifiers_.push_back(input);
        else if(!down && held!=modifiers_.end())capture(input);
    } else if(down)capture(input);
}
void BindingEditor::wheel(int steps) {
    if(capture_ && steps)capture({PhysicalKind::wheel,uint16_t(steps>0 ? 1 : 2)});
}
void BindingEditor::focus(bool focused) {if(!focused)cancel_capture();}
}
