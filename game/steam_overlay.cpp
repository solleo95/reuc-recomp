#include "steam_overlay.h"
#ifndef _WIN32
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#endif
namespace uc {
void SteamOverlay::hint() { if(!failed_) hint_until_=std::chrono::steady_clock::now()+std::chrono::milliseconds(700); }
bool SteamOverlay::open() {
    const auto now=std::chrono::steady_clock::now();
    if(!failed_ && now<hint_until_) return true;
    if(failed_ || now-last_<std::chrono::milliseconds(50)) return open_;
    last_=now;
    open_=query();
    return open_;
}
#ifdef _WIN32
bool SteamOverlay::query() { failed_=true; return false; }
#else
namespace {
// The few Xlib calls this needs, by hand: the sysroot has no libX11 to link and a PC without X must still start.
using Display_=void; using XID=unsigned long;
struct Xlib {
    Display_* (*open)(const char*)=nullptr;
    XID (*root)(Display_*)=nullptr;
    XID (*atom)(Display_*,const char*,int)=nullptr;
    int (*property)(Display_*,XID,XID,long,long,int,XID,XID*,int*,unsigned long*,unsigned long*,unsigned char**)=nullptr;
    int (*free_)(void*)=nullptr;
};
constexpr unsigned long kSteamUiApp=769;   // gamescope's focused app while Steam's own interface is shown
}
bool SteamOverlay::query() {
    static Xlib x; static unsigned long atom=0, blur_atom=0, root=0;
    if(!display_) {
        if(!std::getenv("SteamGameId")) { failed_=true; return false; }   // not started by Steam: no overlay to watch
        void* lib=dlopen("libX11.so.6",RTLD_LAZY);
        if(!lib) { failed_=true; return false; }
        x.open=reinterpret_cast<decltype(x.open)>(dlsym(lib,"XOpenDisplay")); x.root=reinterpret_cast<decltype(x.root)>(dlsym(lib,"XDefaultRootWindow"));
        x.atom=reinterpret_cast<decltype(x.atom)>(dlsym(lib,"XInternAtom")); x.property=reinterpret_cast<decltype(x.property)>(dlsym(lib,"XGetWindowProperty"));
        x.free_=reinterpret_cast<decltype(x.free_)>(dlsym(lib,"XFree"));
        if(!x.open || !x.root || !x.atom || !x.property || !x.free_) { failed_=true; return false; }
        display_=x.open(":0");   // gamescope's main X server (Steam's); the game itself runs on Wayland
        if(!display_) { failed_=true; std::fprintf(stderr,"[steam] gamescope's X server is not reachable: the Steam menus are not detected\n"); return false; }
        root=x.root(display_); atom=x.atom(display_,"GAMESCOPE_FOCUSED_APP",1); blur_atom=x.atom(display_,"GAMESCOPE_BLUR_MODE",1);
        std::fprintf(stderr,"[steam] watching gamescope's focused app (atom %lu) and blur (atom %lu)\n",atom,blur_atom);
    }
    const auto cardinal=[&](unsigned long property) -> unsigned long {   // the first value of a CARDINAL property of the root window, 0 when it is missing
        if(!property) return 0;
        XID type=0; int format=0; unsigned long count=0,after=0; unsigned char* data=nullptr;
        const int status=x.property(display_,root,property,0,1,0,6/*XA_CARDINAL*/,&type,&format,&count,&after,&data);
        const unsigned long value=status==0 && data && format==32 && count>=1 ? *reinterpret_cast<unsigned long*>(data) : 0;
        if(data) x.free_(data);
        return value;
    };
    return cardinal(atom)==kSteamUiApp || cardinal(blur_atom)!=0;
}
#endif
}
