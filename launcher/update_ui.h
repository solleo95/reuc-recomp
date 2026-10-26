#pragma once
#include "update.h"
namespace uc::launcher {
bool draw_update_modal(UpdateController&,std::string_view language,bool game_running,float scale=1.f);
}
