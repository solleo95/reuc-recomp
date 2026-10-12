#pragma once
#include <cstdint>
#include <string>
namespace ppc {
std::string format_insn(uint32_t pc, uint32_t word);
}
