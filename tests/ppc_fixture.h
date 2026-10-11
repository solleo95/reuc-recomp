#pragma once
#include "ppc_rt.h"
#include "guest_mem.h"

struct Fix {
    ppc::Ctx c{};
    Fix() { uc::guest_mem_init(); c.mem = uc::guest_mem_base(); }
};
