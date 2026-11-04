#include "evdev_scan.h"
#include <algorithm>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <string_view>
namespace uc {
namespace {
std::string first_line(const std::string& path) {
    std::ifstream file(path);
    std::string line; std::getline(file,line);
    while(!line.empty() && (line.back()=='\r' || line.back()==' ')) line.pop_back();
    return line;
}
}
bool evdev_pointer_candidate(const EvdevCaps& caps) {
    if(caps.tool_finger || caps.tool_pen || caps.direct || caps.keyboard_keys || !caps.btn_left) return false;
    return (caps.rel_x && caps.rel_y) || (caps.abs_x && caps.abs_y);
}
std::vector<EvdevNode> scan_evdev_nodes() {
    std::vector<EvdevNode> nodes;
    DIR* directory=opendir("/sys/class/input");
    if(!directory) return nodes;
    while(const dirent* entry=readdir(directory)) {
        const std::string_view name=entry->d_name;
        if(name.substr(0,5)!="event" || name.size()==5 || !std::all_of(name.begin()+5,name.end(),[](char c){return c>='0' && c<='9';})) continue;
        const std::string base="/sys/class/input/"+std::string(name)+"/device/";
        EvdevNode node;
        node.index=std::stoi(std::string(name.substr(5)));
        node.path="/dev/input/"+std::string(name);
        std::error_code ignored;
        const auto parent=std::filesystem::canonical(base+"../..",ignored);   // the HID device of a hid-wiimote node (see evdev_group_key)
        const std::string unique=evdev_group_key(ignored ? std::string{} : parent.generic_string(),first_line(base+"phys"));
        node.caps=evdev_caps_from_sysfs(first_line(base+"name"),unique,first_line(base+"capabilities/key"),first_line(base+"capabilities/rel"),
                                        first_line(base+"capabilities/abs"),first_line(base+"properties"),sizeof(long)*8);
        node.role=classify_evdev(node.caps);
        nodes.push_back(std::move(node));
    }
    closedir(directory);
    std::sort(nodes.begin(),nodes.end(),[](const EvdevNode& a,const EvdevNode& b){return a.index<b.index;});
    return nodes;
}
}
