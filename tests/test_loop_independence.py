import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / "scripts"))
import loop_independence as li

HEADER = "C {id} {parent} {fn} {ticks}\nR {reads}\nW {writes}\nH {hles}\n"


def call(id, parent, fn, reads="", writes="", hles="", ticks=100):
    return HEADER.format(id=id, parent=parent, fn=fn, ticks=ticks, reads=reads, writes=writes, hles=hles)


class ParseTest(unittest.TestCase):
    def test_reads_ranges_and_names(self):
        calls = li.parse(call(7, 3, "80383200", "80001000:80001010 80002000:80002004", "80003000:80003008", "void uc::invoke_hle<&hle_GXBegin>(ppc::Ctx &)", 250))
        self.assertEqual(len(calls), 1)
        c = calls[0]
        self.assertEqual((c.id, c.parent, c.fn, c.ticks), (7, 3, "80383200", 250))
        self.assertEqual(c.reads, [(0x80001000, 0x80001010), (0x80002000, 0x80002004)])
        self.assertEqual(c.writes, [(0x80003000, 0x80003008)])
        self.assertEqual(c.hles, ["void uc::invoke_hle<&hle_GXBegin>(ppc::Ctx &)"])


class MalformedTest(unittest.TestCase):
    def test_a_truncated_or_malformed_record_is_skipped_with_its_ranges(self):
        text = "C 1 x\nR 10:20\nW 30:40\nH\n" + call(2, 0, "F", "1000:1010", "2000:2010") + "C 3 0 80000000\nR 1000:\n"   # bad id, a good call, a record cut off at a crash
        calls = li.parse(text)
        self.assertEqual([c.id for c in calls], [2])


class ConflictTest(unittest.TestCase):
    def test_write_against_read_and_write_but_not_read_against_read(self):
        a = li.Call(1, 0, "F", 1, [(100, 200)], [(300, 310)], [])
        b = li.Call(2, 0, "F", 1, [(100, 200), (305, 306)], [(400, 410)], [])   # reads what a wrote at 305
        self.assertEqual(li.conflicts(a, b), [(305, 306)])
        c = li.Call(3, 0, "F", 1, [(100, 150)], [], [])
        self.assertEqual(li.conflicts(a, c), [])   # both only read 100-150


class VerdictTest(unittest.TestCase):
    def test_disjoint_writes_with_shared_reads_are_independent(self):
        text = "".join(call(i, 1, "F", "1000:1100", "%X:%X" % (0x2000 + 0x100 * i, 0x2000 + 0x100 * i + 0x40)) for i in range(2, 6)) + call(1, 0, "P", "", "", "", 1000)
        summary = li.analyse(li.parse(text))["F"]
        self.assertEqual(summary["verdict"], "independent")
        self.assertEqual((summary["calls"], summary["groups"]), (4, 1))

    def test_a_read_of_what_an_earlier_call_wrote_is_dependent(self):
        text = call(2, 1, "F", "", "2000:2040") + call(3, 1, "F", "2020:2024", "3000:3004") + call(1, 0, "P")
        summary = li.analyse(li.parse(text))["F"]
        self.assertEqual(summary["verdict"], "dependent")
        self.assertEqual(summary["example"], (0x2020, 0x2024))

    def test_a_shared_counter_is_a_reduction_not_independence(self):
        text = "".join(call(i, 1, "F", "5000:5004", "5000:5004") for i in range(2, 5)) + call(1, 0, "P")
        summary = li.analyse(li.parse(text))["F"]
        self.assertEqual(summary["verdict"], "reduction")   # every conflict is inside one small range

    def test_gx_calls_make_a_function_impure_and_irq_pairs_need_irq_modelling(self):
        impure = call(2, 1, "F", "", "2000:2004", "void uc::invoke_hle<&hle_GXBegin>(ppc::Ctx &)") + call(3, 1, "F", "", "3000:3004") + call(1, 0, "P")
        self.assertEqual(li.analyse(li.parse(impure))["F"]["verdict"], "impure")
        irq = call(2, 1, "F", "", "2000:2004", "void uc::invoke_hle<&hle_OSDisableInterrupts>(ppc::Ctx &)") + call(3, 1, "F", "", "3000:3004") + call(1, 0, "P")
        self.assertEqual(li.analyse(li.parse(irq))["F"]["verdict"], "needs-irq")

    def test_math_hle_functions_are_memory_only_and_do_not_make_a_function_impure(self):
        math = "void uc::invoke_hle<&hle_PSMTXConcat>(ppc::Ctx &)\tvoid uc::invoke_hle<&hle_ModelHistoryIdentity>(ppc::Ctx &)\tvoid uc::invoke_hle<&hle_Leaf_80139610>(ppc::Ctx &)"
        text = call(2, 1, "F", "", "2000:2004", math) + call(3, 1, "F", "", "3000:3004", math) + call(1, 0, "P")
        self.assertEqual(li.analyse(li.parse(text))["F"]["verdict"], "independent")

    def test_locked_cache_and_mutex_calls_need_os_modelling(self):
        text = call(2, 1, "F", "", "2000:2004", "void uc::invoke_hle<&hle_LCStoreBlocks>(ppc::Ctx &)") + call(3, 1, "F", "", "3000:3004") + call(1, 0, "P")
        self.assertEqual(li.analyse(li.parse(text))["F"]["verdict"], "needs-irq")

    def test_calls_without_a_tracked_caller_are_not_compared_with_each_other(self):
        # parent 0 is "no tracked caller": the calls may come from different frames or threads, so there is no pair to judge
        text = call(2, 0, "F", "", "2000:2004") + call(3, 0, "F", "2000:2004", "3000:3004")
        self.assertEqual(li.analyse(li.parse(text))["F"]["verdict"], "single-call")

    def test_a_function_called_once_per_parent_has_nothing_to_split(self):
        text = call(2, 1, "F", "", "2000:2004") + call(3, 4, "F", "", "2000:2004") + call(1, 0, "P") + call(4, 0, "P")
        self.assertEqual(li.analyse(li.parse(text))["F"]["verdict"], "single-call")


class SiblingTest(unittest.TestCase):
    def test_different_functions_under_one_parent_can_be_checked_against_each_other(self):
        text = call(2, 1, "A", "1000:1010", "2000:2010") + call(3, 1, "B", "2008:200C", "3000:3010") + call(4, 1, "C", "1000:1010", "4000:4010") + call(1, 0, "P")
        pairs = li.sibling_conflicts(li.parse(text))
        self.assertEqual(pairs, {("A", "B"): (0x2008, 0x200C)})


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
