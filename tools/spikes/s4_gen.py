# THROWAWAY spike S4: emits N functions shaped like translator output, 200 per .cpp file.
import pathlib
import sys

n = int(sys.argv[1])
out = pathlib.Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
PER_FILE = 200
REPS = int(sys.argv[3]) if len(sys.argv) > 3 else 18  # statement blocks per function

HEADER = """#include <cstdint>
#include <intrin.h>
struct Ctx { uint32_t r[32]; double f[32]; uint32_t cr, lr, ctr, xer; uint8_t* mem; };
#define LW(c, a) _byteswap_ulong(*(uint32_t*)((c).mem + ((a) & 0x01FFFFFF)))
#define STW(c, a, v) (*(uint32_t*)((c).mem + ((a) & 0x01FFFFFF)) = _byteswap_ulong(v))
"""

def body(i, n):
    lines = [f"void fn_{i}(Ctx& c) {{",
             "  uint32_t r3 = c.r[3], r4 = c.r[4], r5 = c.r[5], r6 = c.r[6]; double f1 = c.f[1], f2 = c.f[2];"]
    for k in range(REPS):
        lines += [f"  r5 = LW(c, r3 + {k * 4}); r6 = r5 + r4 * {k + 3}; STW(c, r3 + {k * 4}, r6 ^ {k});",
                  f"  f1 = f1 * f2 + (double)r6; if ((r6 & {k + 1}) == 0) r4 += {k};"]
    lines += [f"  c.r[3] = r3; c.r[4] = r4; c.f[1] = f1;",
              f"  fn_{(i + 1) % n}(c);", "}"]
    return "\n".join(lines)

for start in range(0, n, PER_FILE):
    idx = range(start, min(start + PER_FILE, n))
    decls = "".join(f"void fn_{(i + 1) % n}(Ctx&);\n" for i in idx)
    src = HEADER + decls + "\n".join(body(i, n) for i in idx) + "\n"
    (out / f"gen_{start // PER_FILE:05d}.cpp").write_text(src)
print(f"wrote {n} functions")
