#pragma once
#include "input_bindings.h"
namespace uc {
class BindingEditor {
public:
    explicit BindingEditor(Bindings bindings=default_bindings());
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
    Bindings bindings_;
    std::optional<Action> capture_;
    bool replace_=false;
    std::vector<PhysicalInput> modifiers_;
    std::optional<std::string> error_;
};
}
