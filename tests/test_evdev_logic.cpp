#include "check.h"
#include "evdev_logic.h"
#include <cmath>
using namespace uc;
using namespace uc::evdev;
static EvdevEvent ev(uint16_t type,uint16_t code,int32_t value) { return {type,code,value}; }
static const EvdevEvent syn=ev(kSyn,kSynReport,0);
static bool near(float a,float b,float tolerance=1e-5f) { return std::fabs(a-b)<=tolerance; }
static uint32_t bit(WiiButton button) { return 1u<<unsigned(button); }
static EvdevCaps mouse_caps() { EvdevCaps c; c.name="Optical Mouse"; c.rel_x=c.rel_y=c.btn_left=true; return c; }
static EvdevCaps gun_caps() { EvdevCaps c; c.name="Sinden Lightgun"; c.abs_x=c.abs_y=c.btn_left=true; c.abs_min={0,0}; c.abs_max={32767,32767}; c.abs_value={16000,16000}; return c; }
static void devices_are_classified() {
    CHECK(classify_evdev(mouse_caps())==EvdevRole::mouse);
    CHECK(classify_evdev(gun_caps())==EvdevRole::gun);
    auto c=gun_caps(); c.abs_max=c.abs_min; CHECK(classify_evdev(c)==EvdevRole::ignore);           // no usable range
    c=gun_caps(); c.tool_finger=true; CHECK(classify_evdev(c)==EvdevRole::ignore);                  // a touchpad
    c=gun_caps(); c.tool_pen=true; CHECK(classify_evdev(c)==EvdevRole::ignore);                     // a tablet
    c=gun_caps(); c.direct=true; CHECK(classify_evdev(c)==EvdevRole::ignore);                       // a touchscreen
    c=gun_caps(); c.btn_left=false; CHECK(classify_evdev(c)==EvdevRole::ignore);                    // no button: not a gun
    c=mouse_caps(); c.btn_left=false; CHECK(classify_evdev(c)==EvdevRole::ignore);
    c=mouse_caps(); c.keyboard_keys=true; c.btn_left=false; CHECK(classify_evdev(c)==EvdevRole::ignore);   // a keyboard
    c=mouse_caps(); c.tool_finger=true; CHECK(classify_evdev(c)==EvdevRole::ignore);                // a touchpad that moves relatively
    EvdevCaps wii; wii.unique="00:11:22:33:44:55";
    wii.name="Nintendo Wii Remote"; CHECK(classify_evdev(wii)==EvdevRole::wiimote_buttons);
    wii.name="Nintendo Wii Remote Accelerometer"; CHECK(classify_evdev(wii)==EvdevRole::wiimote_accel);
    wii.name="Nintendo Wii Remote IR"; CHECK(classify_evdev(wii)==EvdevRole::wiimote_ir);
    wii.name="Nintendo Wii Remote Nunchuk"; CHECK(classify_evdev(wii)==EvdevRole::wiimote_nunchuk);
    for(const char* other:{"Nintendo Wii Remote Motion Plus","Nintendo Wii Remote Pro Controller","Nintendo Wii Remote Balance Board","Nintendo Wii Remote Classic Controller","Nintendo Wii Remote Guitar"}) {
        wii.name=other; CHECK(classify_evdev(wii)==EvdevRole::ignore);
    }
}
static void relative_motion_is_summed_per_frame_and_buttons_follow_it() {
    PointerState state; std::vector<PointerEvent> out;
    const EvdevEvent frame[]={ev(kRel,kRelX,3),ev(kKey,kBtnLeft,1),ev(kRel,kRelY,-2),ev(kRel,kRelX,4),syn};
    feed_pointer(state,mouse_caps(),frame,9,out);
    CHECK(out.size()==2);
    CHECK(out[0].kind==PointerEvent::Kind::relative && out[0].device==9 && out[0].x==7 && out[0].y==-2);   // the motion first...
    CHECK(out[1].kind==PointerEvent::Kind::button && out[1].button==1 && out[1].down);       // ...then the button, though it came in the middle
    out.clear(); const EvdevEvent quiet[]={syn}; feed_pointer(state,mouse_caps(),quiet,9,out); CHECK(out.empty());   // nothing moved: nothing sent, and nothing left over
    const EvdevEvent buttons[]={ev(kKey,kBtnRight,1),ev(kKey,kBtnMiddle,1),ev(kKey,kBtnSide,1),ev(kKey,kBtnExtra,1),ev(kKey,kBtnRight,2),ev(kKey,kBtnLeft,0),syn};
    feed_pointer(state,mouse_caps(),buttons,9,out);
    CHECK(out.size()==5 && out[0].button==3 && out[1].button==2 && out[2].button==4 && out[3].button==5 && out[4].button==1 && !out[4].down);   // a repeat (value 2) is not a press
}
static void the_nodes_of_one_remote_are_grouped_by_their_hid_device() {   // hid-wiimote's accelerometer, IR and nunchuk nodes have no uniq and no phys: their parent is the HID device
    const std::string hid="/sys/devices/pci0000:00/0000:00:14.0/usb1/1-3/1-3:1.0/bluetooth/hci0/hci0:11/0005:057E:0306.0004";
    CHECK(evdev_group_key(hid,"")==hid && evdev_group_key(hid,"aa:bb")==hid);                 // the real device: the HID parent, whatever phys says
    CHECK(evdev_group_key("/sys/devices/virtual","ucfake-wii0")=="ucfake-wii0");              // a virtual (uinput) device has no HID parent of its own: its phys groups its nodes
    CHECK(evdev_group_key("/sys/devices/virtual","").empty());                                  // nothing to group by: not a remote
    CHECK(evdev_group_key("","").empty());
}
static void the_player_led_of_a_led_name_is_its_last_digit_from_p0() {   // hid-wiimote names its LEDs "<hid device>:blue:p0" .. "p3"
    CHECK(evdev_led_player("0005:057E:0306.0004:blue:p0")==0 && evdev_led_player("0005:057E:0306.0004:blue:p3")==3);
    CHECK(evdev_led_player("0005:057E:0306.0004:blue:p4")==-1 && evdev_led_player("0005:057E:0306.0004:blue:p")==-1 && evdev_led_player("input3::capslock")==-1 && evdev_led_player("")==-1);
}
static void the_wheel_is_summed_per_frame_and_follows_the_motion() {
    PointerState state; std::vector<PointerEvent> out;
    const EvdevEvent frame[]={ev(kRel,kRelWheel,-1),ev(kRel,kRelWheel,-1),ev(kRel,kRelHWheel,3),syn};   // two notches towards the user; a sideways wheel is nothing
    feed_pointer(state,mouse_caps(),frame,9,out);
    CHECK(out.size()==1 && out[0].kind==PointerEvent::Kind::wheel && out[0].x==-2 && out[0].device==9);
}
static void absolute_positions_are_normalised_by_the_axis_range() {
    PointerState state; std::vector<PointerEvent> out;
    const EvdevEvent corner[]={ev(kAbs,kAbsX,32767),ev(kAbs,kAbsY,0),syn};
    feed_pointer(state,gun_caps(),corner,4,out);
    CHECK(out.size()==1 && out[0].kind==PointerEvent::Kind::absolute && near(out[0].x,1) && near(out[0].y,0));
    out.clear(); const EvdevEvent middle[]={ev(kAbs,kAbsX,16383),ev(kAbs,kAbsY,16384),syn};
    feed_pointer(state,gun_caps(),middle,4,out); CHECK(out.size()==1 && near(out[0].x,16383/32767.f) && near(out[0].y,16384/32767.f));
    out.clear(); const EvdevEvent only_x[]={ev(kAbs,kAbsX,0),syn};
    feed_pointer(state,gun_caps(),only_x,4,out); CHECK(out.size()==1 && near(out[0].x,0) && near(out[0].y,16384/32767.f));   // an axis that did not change keeps its value
    EvdevCaps shifted=gun_caps(); shifted.abs_min={100,-50}; shifted.abs_max={4195,3995}; shifted.abs_value={100,-50};   // a range that does not start at zero
    PointerState other; out.clear();
    const EvdevEvent range[]={ev(kAbs,kAbsX,2147),ev(kAbs,kAbsY,1972),syn};
    feed_pointer(other,shifted,range,4,out); CHECK(out.size()==1 && near(out[0].x,.5f,1e-3f) && near(out[0].y,.5f,1e-3f));
    out.clear(); const EvdevEvent beyond[]={ev(kAbs,kAbsX,99999),ev(kAbs,kAbsY,-99999),syn};
    feed_pointer(other,shifted,beyond,4,out); CHECK(out.size()==1 && out[0].x==1 && out[0].y==0);   // out of range is clamped, never past 0..1
    out.clear(); const EvdevEvent same[]={ev(kAbs,kAbsX,99999),syn};
    feed_pointer(other,shifted,same,4,out); CHECK(out.empty());                                       // the same position again is no news
}
static void a_shot_lands_at_the_position_it_was_fired_at() {
    PointerState state; std::vector<PointerEvent> out;
    const EvdevEvent frame[]={ev(kKey,kBtnLeft,1),ev(kAbs,kAbsX,0),ev(kAbs,kAbsY,32767),syn};
    feed_pointer(state,gun_caps(),frame,4,out);
    CHECK(out.size()==2 && out[0].kind==PointerEvent::Kind::absolute && near(out[0].x,0) && near(out[0].y,1) && out[1].kind==PointerEvent::Kind::button && out[1].down);
}
static void remote_buttons_and_nunchuk_buttons() {
    WiiEvdevState wii;
    const EvdevEvent down[]={ev(kKey,105,1),ev(kKey,0x130,1),ev(kKey,407,1),ev(kKey,412,1),ev(kKey,0x13c,1),ev(kKey,0x101,1),ev(kKey,0x102,1),ev(kKey,0x131,1),ev(kKey,106,1),ev(kKey,103,1),ev(kKey,108,1),syn};
    feed_wiimote(wii,EvdevRole::wiimote_buttons,down);
    const uint32_t all=bit(WiiButton::left)|bit(WiiButton::a)|bit(WiiButton::plus)|bit(WiiButton::minus)|bit(WiiButton::home)|bit(WiiButton::one)|bit(WiiButton::two)|bit(WiiButton::b)|bit(WiiButton::right)|bit(WiiButton::up)|bit(WiiButton::down);
    CHECK(wii.state.buttons==all);
    const EvdevEvent up[]={ev(kKey,0x130,0),ev(kKey,105,2),syn};   // A released; a repeat changes nothing
    feed_wiimote(wii,EvdevRole::wiimote_buttons,up); CHECK(wii.state.buttons==(all&~bit(WiiButton::a)));
    const EvdevEvent nunchuk[]={ev(kKey,0x135,1),ev(kKey,0x132,1),syn};
    feed_wiimote(wii,EvdevRole::wiimote_nunchuk,nunchuk);
    CHECK((wii.state.buttons&bit(WiiButton::nunchuk_z)) && (wii.state.buttons&bit(WiiButton::nunchuk_c)));
    const EvdevEvent nunchuk_up[]={ev(kKey,0x135,0),syn}; feed_wiimote(wii,EvdevRole::wiimote_nunchuk,nunchuk_up);
    CHECK(!(wii.state.buttons&bit(WiiButton::nunchuk_z)) && (wii.state.buttons&bit(WiiButton::nunchuk_c)));
}
static void accelerometer_stick_and_ir_become_the_protocols_bytes() {
    WiiEvdevState wii;
    const EvdevEvent accel[]={ev(kAbs,kAbsRx,0),ev(kAbs,kAbsRy,-500),ev(kAbs,kAbsRz,100),syn};   // evdev is centred on 0: the remote's own byte is (value+512)/4
    feed_wiimote(wii,EvdevRole::wiimote_accel,accel);
    CHECK(wii.state.accel[0]==128 && wii.state.accel[1]==3 && wii.state.accel[2]==153);
    const EvdevEvent extreme[]={ev(kAbs,kAbsRx,500),ev(kAbs,kAbsRy,-9999),ev(kAbs,kAbsRz,9999),syn};
    feed_wiimote(wii,EvdevRole::wiimote_accel,extreme); CHECK(wii.state.accel[0]==253 && wii.state.accel[1]==0 && wii.state.accel[2]==255);   // clamped to a byte
    const EvdevEvent stick[]={ev(kAbs,kAbsHat0X,-120),ev(kAbs,kAbsHat0X+1,120),syn};
    feed_wiimote(wii,EvdevRole::wiimote_nunchuk,stick); CHECK(wii.state.stick[0]==8 && wii.state.stick[1]==247);   // -120..120 around 128, held inside 8..247: beyond that wii_parse_report takes the bytes for "no nunchuk"
    const EvdevEvent centre[]={ev(kAbs,kAbsHat0X,0),ev(kAbs,kAbsHat0X+1,-9999),syn};
    feed_wiimote(wii,EvdevRole::wiimote_nunchuk,centre); CHECK(wii.state.stick[0]==128 && wii.state.stick[1]==8);
    const EvdevEvent nunchuk_accel[]={ev(kAbs,kAbsRx,-512+4*50),syn};
    feed_wiimote(wii,EvdevRole::wiimote_nunchuk,nunchuk_accel); CHECK(wii.state.nunchuk_accel[0]==50);
    CHECK(!wii.state.ir[0].valid);
    const EvdevEvent dot_x[]={ev(kAbs,kAbsHat0X,500),syn};
    feed_wiimote(wii,EvdevRole::wiimote_ir,dot_x); CHECK(!wii.state.ir[0].valid);                // only one coordinate so far
    const EvdevEvent dot_y[]={ev(kAbs,kAbsHat0X+1,300),ev(kAbs,kAbsHat0X+2,1023),ev(kAbs,kAbsHat0X+3,1023),ev(kAbs,kAbsHat0X+6,10),ev(kAbs,kAbsHat0X+7,20),syn};
    feed_wiimote(wii,EvdevRole::wiimote_ir,dot_y);
    CHECK(wii.state.ir[0].valid && wii.state.ir[0].x==500 && wii.state.ir[0].y==300);
    CHECK(!wii.state.ir[1].valid);                                                                // 1023,1023 is "not seen"
    CHECK(!wii.state.ir[2].valid && wii.state.ir[3].valid && wii.state.ir[3].x==10 && wii.state.ir[3].y==20);
    const EvdevEvent gone[]={ev(kAbs,kAbsHat0X,1023),ev(kAbs,kAbsHat0X+1,1023),syn};
    feed_wiimote(wii,EvdevRole::wiimote_ir,gone); CHECK(!wii.state.ir[0].valid);
    const EvdevEvent back[]={ev(kAbs,kAbsHat0X,640),ev(kAbs,kAbsHat0X+1,384),syn};
    feed_wiimote(wii,EvdevRole::wiimote_ir,back); CHECK(wii.state.ir[0].valid && wii.state.ir[0].x==640 && wii.state.ir[0].y==384);
}
static void what_the_nodes_say_survives_the_protocols_report() {   // the nodes are turned into a report so that the existing protocol, pointer and swing code reads them unchanged
    WiiEvdevState wii; wii.state.has_nunchuk=true;
    feed_wiimote(wii,EvdevRole::wiimote_buttons,std::vector<EvdevEvent>{ev(kKey,0x130,1),ev(kKey,0x13c,1),syn});
    feed_wiimote(wii,EvdevRole::wiimote_accel,std::vector<EvdevEvent>{ev(kAbs,kAbsRx,40),ev(kAbs,kAbsRy,-40),ev(kAbs,kAbsRz,104),syn});
    feed_wiimote(wii,EvdevRole::wiimote_ir,std::vector<EvdevEvent>{ev(kAbs,kAbsHat0X,300),ev(kAbs,kAbsHat0X+1,400),ev(kAbs,kAbsHat0X+2,700),ev(kAbs,kAbsHat0X+3,410),syn});
    feed_wiimote(wii,EvdevRole::wiimote_nunchuk,std::vector<EvdevEvent>{ev(kAbs,kAbsHat0X,60),ev(kAbs,kAbsHat0X+1,-60),ev(kKey,0x135,1),syn});
    WiiState back; CHECK(wii_parse_report(wii_encode_report(wii.state),back));
    CHECK(back.buttons==wii.state.buttons && back.accel==wii.state.accel && back.stick==wii.state.stick);
    CHECK(back.ir[0].valid && back.ir[0].x==300 && back.ir[0].y==400 && back.ir[1].valid && back.ir[1].x==700 && back.ir[1].y==410 && !back.ir[2].valid);
}
static void sysfs_bitmaps_tell_what_a_device_is_without_opening_it() {   // /sys/class/input/eventN/device/capabilities/* is readable by everyone: a keyboard never has to be opened
    CHECK(evdev_bitmap_has("10000 0 0 0 0",0x110));                      // five 64-bit words, the highest first: bit 0x110 = word 4, bit 16
    CHECK(!evdev_bitmap_has("10000 0 0 0 0",0x111) && !evdev_bitmap_has("10000 0 0 0 0",3));
    CHECK(evdev_bitmap_has("3",0) && evdev_bitmap_has("3",1) && !evdev_bitmap_has("3",2) && !evdev_bitmap_has("3",64));   // one word; beyond the string is not set
    CHECK(evdev_bitmap_has("1 0",64) && !evdev_bitmap_has("1 0",0));
    CHECK(!evdev_bitmap_has("100000000 0",32,32) && evdev_bitmap_has("1 0",32,32));                                       // 32-bit words (an ARM board): "1 0" has bit 32 and "100000000" is out of a word
    CHECK(!evdev_bitmap_has("",0) && !evdev_bitmap_has("zz",0) && !evdev_bitmap_has("1",100000));                        // junk is "no"
    // evdev_caps_from_sysfs(name,unique,key,rel,abs,properties): the bitmaps of capabilities/key, rel, abs and the device's properties file
    const auto mouse=evdev_caps_from_sysfs("Optical Mouse","","1f0000 0 0 0 0","3","0","0");   // five buttons, relative x and y
    CHECK(mouse.rel_x && mouse.rel_y && mouse.btn_left && !mouse.abs_x && !mouse.keyboard_keys && classify_evdev(mouse)==EvdevRole::mouse);
    auto gun=evdev_caps_from_sysfs("Gun","","10000 0 0 0 0","0","3","0");                      // absolute x and y, one button
    CHECK(gun.abs_x && gun.abs_y && gun.btn_left && !gun.rel_x);
    CHECK(classify_evdev(gun)==EvdevRole::ignore);                                               // the range comes from EVIOCGABS after opening: until then it is not a gun
    gun.abs_max={32767,32767}; CHECK(classify_evdev(gun)==EvdevRole::gun);
    CHECK(evdev_caps_from_sysfs("Keyboard","","ffffffff ffffffff ffffffff ffffffff","0","0","0").keyboard_keys);   // KEY_A (30) is set
    CHECK(!mouse.tool_finger && !mouse.tool_pen && !mouse.direct);
    CHECK(evdev_caps_from_sysfs("Touchpad","","20 0 0 0 0 0","0","3","0").tool_finger);          // BTN_TOOL_FINGER 0x145 = word 5, bit 5
    CHECK(evdev_caps_from_sysfs("Tablet","","1 0 0 0 0 0","0","3","0").tool_pen);                // BTN_TOOL_PEN 0x140 = word 5, bit 0
    CHECK(evdev_caps_from_sysfs("Screen","","0","0","3","2").direct && !evdev_caps_from_sysfs("Pointer","","0","0","3","1").direct);   // property bit 1 is INPUT_PROP_DIRECT, bit 0 INPUT_PROP_POINTER
    CHECK(evdev_caps_from_sysfs("Gun","","10000 0 0 0 0","0","3","0",32).btn_left==false);       // the same text read as 32-bit words is another bit
}
int main() {
    sysfs_bitmaps_tell_what_a_device_is_without_opening_it();
    devices_are_classified(); the_nodes_of_one_remote_are_grouped_by_their_hid_device(); the_player_led_of_a_led_name_is_its_last_digit_from_p0(); the_wheel_is_summed_per_frame_and_follows_the_motion(); relative_motion_is_summed_per_frame_and_buttons_follow_it(); absolute_positions_are_normalised_by_the_axis_range();
    a_shot_lands_at_the_position_it_was_fired_at(); remote_buttons_and_nunchuk_buttons(); accelerometer_stick_and_ir_become_the_protocols_bytes();
    what_the_nodes_say_survives_the_protocols_report();
}
