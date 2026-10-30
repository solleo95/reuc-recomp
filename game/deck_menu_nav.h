#pragma once
#include "settings_registry.h"
#include <algorithm>
#include <span>
#include <vector>
namespace uc {
// What the Steam Deck menu does with a pad, a keyboard or a tap, apart from drawing: the focused page and row, an open choice list, and what each key changes. The menu draws
// from this state and feeds taps in as the same keys, so a pad and a finger always agree.
enum class NavKey : uint8_t { up, down, left, right, accept, back, page_prev, page_next, add, clear };   // add (X) and clear (Y): the Buttons page only
struct NavResult { bool changed=false; bool close=false; };
// The items of a page that are visible with these options (a row can hide itself: the reload zone's size while the zone is off).
inline std::vector<const settings::Item*> deck_rows(settings::Page page,const Options& options,const settings::Env& env) {
    std::vector<const settings::Item*> rows;
    for(const auto* item:settings::items_for(settings::deck_menu,page)) if(!item->state || item->state(options,env).visible) rows.push_back(item);
    return rows;
}
struct DeckNav {
    int page=0, row=0;            // the focused page (an index into the pages) and row
    bool list_open=false;         // a choice's list is open on the focused row
    int list_index=0;             // the highlighted entry of the list
    static bool enabled(const settings::Item& item,const Options& options,const settings::Env& env) { return !item.state || item.state(options,env).enabled; }
    void keep_inside(int rows,int pages) { page=std::clamp(page,0,std::max(0,pages-1)); row=std::clamp(row,0,std::max(0,rows-1)); if(!rows) list_open=false; }
    // `rows`: the visible items of the focused page. Changes go into `options`; the caller applies them.
    NavResult handle(NavKey key,Options& options,const settings::Env& env,std::span<const settings::Item* const> rows,int pages) {
        NavResult result;
        keep_inside(int(rows.size()),pages);
        const settings::Item* item=rows.empty() ? nullptr : rows[size_t(row)];
        if(list_open && item) {
            const int count=int(item->labels(options,env,[](std::string_view) { return std::string(); }).size());
            switch(key) {
            case NavKey::up: list_index=std::max(0,list_index-1); break;
            case NavKey::down: list_index=std::min(count-1,list_index+1); break;
            case NavKey::accept: result.changed=settings::set_choice(*item,options,env,list_index); list_open=false; break;
            case NavKey::back: list_open=false; break;
            default: break;
            }
            return result;
        }
        switch(key) {
        case NavKey::up: row=std::max(0,row-1); break;
        case NavKey::down: row=std::min(int(rows.size())-1,row+1); break;
        case NavKey::left: case NavKey::right:
            if(item && enabled(*item,options,env)) result.changed=settings::step(*item,options,env,key==NavKey::left ? -1 : 1);
            break;
        case NavKey::accept:
            if(!item || !enabled(*item,options,env)) break;
            if(item->kind==settings::Kind::toggle) result.changed=settings::flip(*item,options);
            else if(item->kind==settings::Kind::choice) { list_open=true; list_index=std::max(0,item->index(options,env)); }
            else if(item->kind==settings::Kind::color) {   // the presets go round
                result.changed=settings::step(*item,options,env,1);
                if(!result.changed) { item->set_color(options,settings::color_presets()[0]); result.changed=true; }
            }
            break;
        case NavKey::back: result.close=true; break;
        case NavKey::page_prev: if(page>0) { --page; row=0; } break;
        case NavKey::page_next: if(page+1<pages) { ++page; row=0; } break;
        case NavKey::add: case NavKey::clear: break;
        }
        return result;
    }
};
}
