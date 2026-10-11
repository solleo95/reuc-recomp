#include "ppc/isa.h"
#include "translator/emit.h"
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    uc::Program p; uc::EmitStats st;
    uc::Function f{0x80003100, 0x80003200};
    std::FILE* o = std::fopen(argv[1], "w");
    if (!o) return 1;
    std::fprintf(o, "#include \"ppc_rt.h\"\nvoid opcheck(ppc::Ctx& c) {\n");
    size_t n = 0;
    auto* const* ops = ppc::all_ops(n);
    for (size_t i = 0; i < n; ++i) {
        const ppc::OpInfo& op = *ops[i];
        using ppc::Kind;
        if (op.kind == Kind::BR || op.kind == Kind::BC || op.kind == Kind::BCLR || op.kind == Kind::BCCTR || op.kind == Kind::SC || op.kind == Kind::UNSUP) continue;
        uint32_t w = uint32_t(op.primary) << 26 | (op.sub == ppc::Sub::N ? 0u : uint32_t(op.xo) << 1);
        std::string s = uc::emit_insn(p, f, 0x80003100, w, st);
        if (s.find("ppc::op_") != 0) continue;   // traps (OE forms, twi 31) are not op calls
        std::fprintf(o, "  %s\n", s.c_str());
    }
    std::fprintf(o, "}\n");
    std::fclose(o);
    return 0;
}
