#pragma once
#include "input_config.h"
#include "options.h"
namespace uc {
Options options_from_config(const ConfigDocument&);
void store_options(ConfigDocument&,const Options&);
}
