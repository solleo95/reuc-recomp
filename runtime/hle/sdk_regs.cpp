// The SDK's register save/restore helpers (the compiler's _savegpr_N, _restgpr_N, _savefpr_N, _restfpr_N, N = 14..31): every function prologue and epilogue of the
// game calls one, with r11 pointing just above the save area. In the translated code each is a chain of one-instruction functions that fall into each other (up to
// 18 nested calls); here an entry is one native function running the same load/store instructions in the same order, so the guest memory, the registers and the
// write watch behave exactly as before. project/hle_map.txt binds the 72 entry addresses; tests/test_sdk_regs_hle.cpp checks them against the instruction sequences.
#include "ppc_rt.h"
#include <utility>

namespace {
using ppc::Ctx;
// The general registers are one contiguous block of 4-byte words ending at r11. When the whole block lies outside the MMIO window (the stack: always) it is stored or
// loaded in one pass: the MMIO test and the write barrier run once for the block instead of once per word (the barrier marks the same pages). Anything else, including
// an address that wraps, takes the word-by-word path with its exact per-word behaviour.
template<int First> bool gpr_block(Ctx& c, uint32_t& ea) {
    constexpr uint32_t bytes = 4 * (32 - First);
    ea = c.r[11] - bytes;
    return !ppc::is_mmio(ea) && !ppc::is_mmio(ea + bytes - 1) && ea + bytes > ea;
}
template<int First, int... I> void save_gpr(Ctx& c, std::integer_sequence<int,I...>) {
    uint32_t ea;
    if (gpr_block<First>(c, ea)) [[likely]] {
        for (uint32_t o = 0; o < 4 * (32 - First); o += 4) UC_MT_WRITE(ea + o, 4);
        gx::note_write_range(ea, 4 * (32 - First));
        const uint32_t words[32 - First] = {_byteswap_ulong(c.r[First + I])...};
        std::memcpy(ppc::hp(c, ea), words, sizeof words);
        return;
    }
    (ppc::op_stw(c, First + I, 11, -4 * (32 - First - I)), ...);
}
template<int First, int... I> void rest_gpr(Ctx& c, std::integer_sequence<int,I...>) {
    uint32_t ea;
    if (gpr_block<First>(c, ea)) [[likely]] {
        for (uint32_t o = 0; o < 4 * (32 - First); o += 4) UC_MT_READ(ea + o, 4);
        uint32_t words[32 - First];
        std::memcpy(words, ppc::hp(c, ea), sizeof words);
        ((c.r[First + I] = _byteswap_ulong(words[I])), ...);
        return;
    }
    (ppc::op_lwz(c, First + I, 11, -4 * (32 - First - I)), ...);
}
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
