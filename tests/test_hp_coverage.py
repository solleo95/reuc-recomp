import re, sys, unittest
from pathlib import Path

HEADER = Path(sys.argv[1]) / "runtime" / "ppc_rt.h"


class HpCoverageTest(unittest.TestCase):
    def test_every_guest_memory_access_of_ppc_rt_has_a_trace_hook(self):
        """The tracer (runtime/mem_trace.h) only sees what ppc_rt.h tells it: a new load or store written with hp() but without UC_MT_READ / UC_MT_WRITE would be invisible and
        make a function look independent that is not."""
        lines = HEADER.read_text(encoding="utf-8").splitlines()
        missing = []
        for index, line in enumerate(lines):
            if "hp(c" not in line or "PPC_INLINE uint8_t* hp(" in line:
                continue
            nearby = "\n".join(lines[max(0, index - 3):index + 1])
            if not re.search(r"UC_MT_(READ|WRITE)\(", nearby):
                missing.append((index + 1, line.strip()[:100]))
        self.assertEqual(missing, [])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
