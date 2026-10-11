# THROWAWAY spike S1 helper. Usage: python -I s1_zero_runs.py <main.dol>
# Lists zero runs >= 32 words inside DOL text sections and counts middleware/SDK strings.
import re
import struct
import sys

f = open(sys.argv[1], "rb").read()
off = struct.unpack(">18I", f[0x00:0x48])
addr = struct.unpack(">18I", f[0x48:0x90])
size = struct.unpack(">18I", f[0x90:0xD8])

for i in range(7):  # text sections only
    if not size[i]:
        continue
    n = size[i] // 4
    w = struct.unpack(">%dI" % n, f[off[i]:off[i] + n * 4])
    k = 0
    while k < n:
        if w[k] == 0:
            j = k
            while j < n and w[j] == 0:
                j += 1
            if j - k >= 32:
                print("zero run %08X..%08X words=%d" % (addr[i] + k * 4, addr[i] + j * 4, j - k))
            k = j
        else:
            k += 1

for pat in [rb"CRI [A-Z]+/WII[\x20-\x7e]{0,30}", rb"ADX[A-Z]*/WII[\x20-\x7e]{0,30}", rb"[\x20-\x7e]{0,12}TRK[\x20-\x7e]{0,30}",
            rb"<< RVL_SDK - [A-Z]+"]:
    found = sorted(set(m.group(0) for m in re.finditer(pat, f)))
    print(pat.decode(errors="replace"), len(found), found[:8])
