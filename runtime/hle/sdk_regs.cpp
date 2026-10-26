// The SDK's register save/restore helpers (the compiler's _savegpr_N, _restgpr_N, _savefpr_N, _restfpr_N, N = 14..31): every function prologue and epilogue of the
// game calls one, with r11 pointing just above the save area. In the translated code each is a chain of one-instruction functions that fall into each other (up to
// 18 nested calls); here an entry is one native function running the same load/store instructions in the same order, so the guest memory, the registers and the
// write watch behave exactly as before. project/hle_map.txt binds the 72 entry addresses; tests/test_sdk_regs_hle.cpp checks them against the instruction sequences.
#include "ppc_rt.h"
#include <utility>

namespace {
using ppc::Ctx;
template<int First, int... I> void save_gpr(Ctx& c, std::integer_sequence<int,I...>) { (ppc::op_stw(c, First + I, 11, -4 * (32 - First - I)), ...); }
template<int First, int... I> void rest_gpr(Ctx& c, std::integer_sequence<int,I...>) { (ppc::op_lwz(c, First + I, 11, -4 * (32 - First - I)), ...); }
template<int First, int... I> void save_fpr(Ctx& c, std::integer_sequence<int,I...>) { (ppc::op_stfd(c, First + I, 11, -8 * (32 - First - I)), ...); }
template<int First, int... I> void rest_fpr(Ctx& c, std::integer_sequence<int,I...>) { (ppc::op_lfd(c, First + I, 11, -8 * (32 - First - I)), ...); }
}  // namespace

#define UC_REG_HLE(N) \
    void hle_SaveGpr##N(Ctx& c) { save_gpr<N>(c, std::make_integer_sequence<int, 32 - N>{}); } \
    void hle_RestGpr##N(Ctx& c) { rest_gpr<N>(c, std::make_integer_sequence<int, 32 - N>{}); } \
    void hle_SaveFpr##N(Ctx& c) { save_fpr<N>(c, std::make_integer_sequence<int, 32 - N>{}); } \
    void hle_RestFpr##N(Ctx& c) { rest_fpr<N>(c, std::make_integer_sequence<int, 32 - N>{}); }
UC_REG_HLE(14) UC_REG_HLE(15) UC_REG_HLE(16) UC_REG_HLE(17) UC_REG_HLE(18) UC_REG_HLE(19) UC_REG_HLE(20) UC_REG_HLE(21) UC_REG_HLE(22)
UC_REG_HLE(23) UC_REG_HLE(24) UC_REG_HLE(25) UC_REG_HLE(26) UC_REG_HLE(27) UC_REG_HLE(28) UC_REG_HLE(29) UC_REG_HLE(30) UC_REG_HLE(31)
