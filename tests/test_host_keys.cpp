#include "check.h"
#include "host_keys.h"
int main() {
    uc::HostKeys keys;
    CHECK(uc::host_key(keys,SDL_SCANCODE_W,false)==uc::HostKeyResult::none && !keys.menu_open && !keys.paused);   // game keys pass through
    CHECK(uc::host_key(keys,SDL_SCANCODE_INSERT,false)==uc::HostKeyResult::menu_toggled && keys.menu_open);
    CHECK(uc::host_key(keys,SDL_SCANCODE_P,false)==uc::HostKeyResult::consumed && !keys.paused);   // P does nothing while the menu is open
    CHECK(uc::host_key(keys,SDL_SCANCODE_INSERT,true)==uc::HostKeyResult::none && keys.menu_open);   // a binding capture gets Insert
    CHECK(uc::host_key(keys,SDL_SCANCODE_P,true)==uc::HostKeyResult::none);                           // ... and P
    CHECK(uc::host_key(keys,SDL_SCANCODE_INSERT,false)==uc::HostKeyResult::menu_toggled && !keys.menu_open);
    CHECK(uc::host_key(keys,SDL_SCANCODE_P,false)==uc::HostKeyResult::pause_toggled && keys.paused);
    CHECK(uc::host_key(keys,SDL_SCANCODE_INSERT,false)==uc::HostKeyResult::menu_toggled && keys.menu_open && keys.paused);   // the menu over a paused game
    CHECK(uc::host_key(keys,SDL_SCANCODE_INSERT,false)==uc::HostKeyResult::menu_toggled && !keys.menu_open && keys.paused);
    CHECK(uc::host_key(keys,SDL_SCANCODE_P,false)==uc::HostKeyResult::pause_toggled && !keys.paused);
    CHECK(uc::host_key(keys,SDL_SCANCODE_ESCAPE,false)==uc::HostKeyResult::none);   // the game's Escape while the menu is closed
    uc::host_key(keys,SDL_SCANCODE_INSERT,false);
    CHECK(uc::host_key(keys,SDL_SCANCODE_ESCAPE,true)==uc::HostKeyResult::none && keys.menu_open);   // cancels a capture, not the menu
    CHECK(uc::host_key(keys,SDL_SCANCODE_ESCAPE,false)==uc::HostKeyResult::menu_toggled && !keys.menu_open);
    {   // a rebound menu key: Insert is an ordinary key again; none bound keeps Insert so the menu can always be opened
        const SDL_Scancode bound[]={SDL_SCANCODE_F10,SDL_SCANCODE_GRAVE};
        CHECK(uc::host_key(keys,SDL_SCANCODE_INSERT,false,bound)==uc::HostKeyResult::none && !keys.menu_open);
        CHECK(uc::host_key(keys,SDL_SCANCODE_GRAVE,false,bound)==uc::HostKeyResult::menu_toggled && keys.menu_open);
        CHECK(uc::host_key(keys,SDL_SCANCODE_F10,false,bound)==uc::HostKeyResult::menu_toggled && !keys.menu_open);
        CHECK(uc::host_key(keys,SDL_SCANCODE_F10,true,bound)==uc::HostKeyResult::none && !keys.menu_open);   // a capture gets the menu key too
        CHECK(uc::is_menu_key(SDL_SCANCODE_INSERT,{}) && !uc::is_menu_key(SDL_SCANCODE_F10,{}));
    }
}
