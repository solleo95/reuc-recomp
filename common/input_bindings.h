#pragma once
#include <array>
#include <bitset>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace uc {
// Knife tuning: V swings from the lower left to the upper right (screen y points down). A mouse swing is a fast mouse movement: the motion of the last kKnifeWindowNs
// (raw counts) must move at kKnifeSpeed counts per second or more and be at least kKnifeMinLength long (a floor against timer noise on single events).
constexpr float kKnifeKeyDirectionX=0.70710678f,kKnifeKeyDirectionY=-0.70710678f,kKnifeSpeed=4000.f,kKnifeMinLength=12.f;
constexpr uint64_t kKnifeWindowNs=60000000ull,kKnifeReportNs=8000000ull;
// Mouse shake (motion QTE): a step is a reversal of at least kShakeStroke counts; the mouse must have moved kShakeMinPath counts within about kShakePathSeconds
// (a slow wave is aiming) and may pause at most kShakeGapNs between reports.
constexpr double kShakeStroke=20,kShakeMinPath=24,kShakePathSeconds=.06;
constexpr uint64_t kShakeGapNs=100000000ull;
enum class Action:uint8_t { interact,fire,weapon_previous,weapon_next,reload,grenade,knife,pause,confirm,back,release_cursor,screenshot,qte_shake,count };
enum class InputContext:uint8_t { inactive,menu,gameplay,qte };
enum class PhysicalKind:uint8_t { key,mouse,wheel };
struct PhysicalInput { PhysicalKind kind=PhysicalKind::key; uint16_t code=0; bool operator==(const PhysicalInput&) const=default; };
enum class BindingKind:uint8_t { physical,interact_fire,interact_motion,qte_motion };
struct Binding { BindingKind kind=BindingKind::physical; PhysicalInput input; std::vector<PhysicalInput> modifiers; bool operator==(const Binding&) const=default; };
using Bindings=std::array<std::vector<Binding>,size_t(Action::count)>;
Bindings default_bindings();
std::optional<std::string> validate_bindings(const Bindings&);
struct ActionEvent { Action action; bool down; float direction_x=0,direction_y=0; bool pulse=false; };
struct ActionFrame { uint64_t held=0,pressed=0,released=0,generation=0; std::array<ActionEvent,256> events{}; size_t count=0; };
class BindingResolver {
public:
    void replace(const Bindings&);
    void context(InputContext);
    void qte_window(bool);
    void focus(bool);
    void button(PhysicalInput,bool);
    void wheel(int);
    void motion(float,float,uint64_t timestamp_ns=0);
    ActionFrame consume();
private:
    bool allowed(Action) const;
    bool held(PhysicalInput) const;
    bool matches(const Binding&,PhysicalInput) const;
    bool special(Action,BindingKind) const;
    uint64_t physical_actions() const;
    void cancel();
    void finish_motion();
    void qte_motion(float,float,uint64_t);
    void emit(Action,bool,float=0,float=0,bool=false);
    void pulse(Action,float=0,float=0);
    Bindings bindings_=default_bindings();
    InputContext context_=InputContext::inactive;
    bool focused_=false,suppress_fire_=false,qte_window_=false;
    std::bitset<512> keys_,blocked_keys_;
    std::bitset<256> mouse_,blocked_mouse_;
    uint64_t raw_actions_=0,canceled_releases_=0;
    ActionFrame frame_;
    std::optional<size_t> motion_event_;
    double motion_x_=0,motion_y_=0;
    uint64_t stroke_time_=0,stroke_lead_=0,last_motion_time_=0;
    struct Shake {uint64_t last=0; bool stroke=false; double dx=0,dy=0,proj=0,peak=0,back_x=0,back_y=0,path=0;} shake_;
};
}
