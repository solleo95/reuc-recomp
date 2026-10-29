#pragma once
#include "input_bindings.h"
namespace uc {
// Which table the editor edits: the keyboard and mouse table (the Controls tab), the gamepad table or the Wii Remote table (the Controllers tab). Each table takes only
// its own inputs, has its own defaults, and the gamepad and Wii Remote tables have no modifier keys: a pressed button is a binding at once.
enum class BindingTable:uint8_t { keyboard,gamepad,wiimote };
class BindingEditor {
public:
    explicit BindingEditor(Bindings bindings=default_bindings(),BindingTable table=BindingTable::keyboard);
    const Bindings& bindings() const {return bindings_;}
    std::optional<Action> capturing() const {return capture_;}
    const std::optional<std::string>& error() const {return error_;}
    std::optional<std::string> add(Action,const Binding&);
    void remove(Action,size_t);
    void clear(Action);
    void restore_defaults();
    void begin_capture(Action,bool replace=false);
    void cancel_capture();
    void event(PhysicalInput,bool);
    void wheel(int);
    void focus(bool);
private:
    void capture(PhysicalInput);
    std::optional<std::string> validate(const Bindings&) const;
    BindingTable table_;
    Bindings bindings_;
    std::optional<Action> capture_;
    bool replace_=false;
    std::vector<PhysicalInput> modifiers_;
    std::optional<std::string> error_;
};
}
