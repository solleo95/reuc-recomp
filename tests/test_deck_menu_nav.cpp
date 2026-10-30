#include "check.h"
#include "deck_menu_nav.h"
using namespace uc;
using namespace uc::settings;
int main() {
    Env env; Options o;
    const auto pages=pages_for(deck_menu);
    const int page_count=int(pages.size());
    DeckNav nav;
    auto rows=[&] { return deck_rows(pages[size_t(nav.page)],o,env); };
    auto press=[&](NavKey key) { const auto r=rows(); return nav.handle(key,o,env,r,page_count); };
    {   // pages and rows
        CHECK(pages.front()==Page::graphics);
        press(NavKey::page_prev); CHECK(nav.page==0);
        press(NavKey::page_next); CHECK(nav.page==1 && nav.row==0);
        for(int i=0;i<10;++i) press(NavKey::page_next);
        CHECK(nav.page==page_count-1);
        nav.page=0; press(NavKey::up); CHECK(nav.row==0);
        for(int i=0;i<100;++i) press(NavKey::down);
        CHECK(nav.row==int(rows().size())-1);   // stops at the last row
    }
    {   // a toggle: right = on, left = off, accept flips
        nav=DeckNav{}; nav.page=int(std::find(pages.begin(),pages.end(),Page::effects)-pages.begin());
        const auto list=rows(); int at=-1; for(size_t i=0;i<list.size();++i) if(std::string(list[i]->id)=="per_pixel_lighting") at=int(i);
        CHECK(at>=0); nav.row=at;
        o.effects.per_pixel_lighting=false;
        CHECK(press(NavKey::accept).changed && o.effects.per_pixel_lighting);
        CHECK(press(NavKey::accept).changed && !o.effects.per_pixel_lighting);
        CHECK(press(NavKey::right).changed && o.effects.per_pixel_lighting && !press(NavKey::right).changed);
        CHECK(press(NavKey::left).changed && !o.effects.per_pixel_lighting);
    }
    {   // a choice: accept opens the list, up/down move in it, accept picks, back closes it without a change
        nav=DeckNav{}; nav.page=0;
        const auto list=rows(); int at=-1; for(size_t i=0;i<list.size();++i) if(std::string(list[i]->id)=="fps_limit") at=int(i);
        CHECK(at>=0); nav.row=at; o.fps_limit=60;
        CHECK(!press(NavKey::accept).changed && nav.list_open && nav.list_index==1);
        press(NavKey::down); CHECK(nav.list_index==2);
        const auto before=o.fps_limit; press(NavKey::back); CHECK(!nav.list_open && o.fps_limit==before);   // back closes the list, not the menu
        CHECK(!press(NavKey::back).changed);
        press(NavKey::accept); press(NavKey::down); CHECK(press(NavKey::accept).changed && !nav.list_open && o.fps_limit!=before);
        CHECK(press(NavKey::back).close);   // with no list open back closes the menu
        nav.list_open=false; o.fps_limit=30; press(NavKey::left); CHECK(o.fps_limit==30);   // left of the first entry
        press(NavKey::right); CHECK(o.fps_limit==60);
    }
    {   // a slider moves by its step; a greyed-out row does not change; a hidden row leaves the list
        nav=DeckNav{}; nav.page=int(std::find(pages.begin(),pages.end(),Page::effects)-pages.begin());
        auto list=rows(); int at=-1; for(size_t i=0;i<list.size();++i) if(std::string(list[i]->id)=="bloom_strength") at=int(i);
        CHECK(at>=0); nav.row=at;
        o.bloom=true; o.bloom_strength=100; press(NavKey::right); CHECK(o.bloom_strength==105);
        o.bloom=false; CHECK(!press(NavKey::right).changed && o.bloom_strength==105);   // follows the bloom switch
        nav.page=int(std::find(pages.begin(),pages.end(),Page::gamepad)-pages.begin());   // the Deck's Controls page
        o.reload_zone=ReloadZone::off; o.pointer=PointerMode::absolute;
        bool has_size=false; for(const auto* item:rows()) has_size|=std::string(item->id)=="reload_zone_size";
        CHECK(!has_size);
        o.reload_zone=ReloadZone::corner; has_size=false; for(const auto* item:rows()) has_size|=std::string(item->id)=="reload_zone_size";
        CHECK(has_size);
        nav.row=1000; press(NavKey::up); CHECK(nav.row<int(rows().size()));   // a row that went away is not left dangling
    }
    {   // a colour: accept goes round the presets
        nav=DeckNav{}; nav.page=int(std::find(pages.begin(),pages.end(),Page::reticle)-pages.begin());
        const auto list=rows(); int at=-1; for(size_t i=0;i<list.size();++i) if(std::string(list[i]->id)=="reticle_color_p1") at=int(i);
        CHECK(at>=0); nav.row=at; o.reticle.color_p1=0;
        const size_t n=color_presets().size();
        for(size_t i=1;i<n;++i) { press(NavKey::accept); CHECK(o.reticle.color_p1==color_presets()[i]); }
        press(NavKey::accept); CHECK(o.reticle.color_p1==0);
    }
    return 0;
}
