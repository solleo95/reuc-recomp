import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(sys.argv[1]) / "scripts"))
from uctest import language

# A REAL --test-server run log (build/autotest/1793816552115469100/run.log, English launcher, no --log-dvd: the diagnostic directory logs the
# opens but not the reads). Verbatim, with the unrelated lines around them.
REAL_ENGLISH_LOG = """\
[session] app=game pid=51872 UTC=2026-11-04T18:22:32Z executable=D:\\Projects\\Native Ports\\Umbrella-chronicles-recomp\\dist\\uc.exe
[system] Windows 10.0 build 26200 CPU=AMD Ryzen 9 7950X 16-Core Processor logical=32 RAM_total_MB=65180 build=Nov  3 2026 20:31:24
[dvd] open Data/HomeButton2/homeBtn_ENG.arc
[dvd] open Data/HomeButton2/SpeakerSe.arc
[dvd] open Data/HomeButton2/home_nosave.csv
[dvd] open Data/HomeButton2/config.txt
[dvd] open Data/HomeButton2/HomeButtonSe.arc
[dvd] open Data/sound/biosound.brsar
[dvd] open Data/sound/SNDEFF.dat
[dvd] open Data/sysres/font0/utf8_E.brfnt
[dvd] open Data/sysres/font1/utf8_E.brfnt
[dvd] open Data/sysres/font2/utf8_E.brfnt
[dvd] open Data/message/mes_us.txtres
[dvd] open Data/layout/dialog_us.arc
[dvd] open Data/sysres/vib.mot
[dvd] open Data/script/r_biouc.arc
[dvd] open Data/layout/atract_4_3_us.arc
[dvd] open Data/sysres_g.arc
[dvd] open Data/message/movie_jimaku.csvtbl
[dvd] open Data/layout/game_us.arc
[dvd] open Data/effect/ef_com.arc
[session] game exit=0
"""

# SYNTHETIC, for the parser/check logic only: the Spanish run is not recorded yet. Every path is a real RBUE08 disc file
# (S:\\Games\\Umbrella Chronicles\\game\\DATA\\files\\Data) and mes_sp.txtres is 238080 bytes (task-1 results). The read line is the
# runtime/hle/dvd.cpp --log-dvd format. Live evidence for this pack comes from the actual game run, not from this fixture.
SYNTHETIC_SPANISH_LOG = """\
#[synthetic]
[dvd] open Data/sysres/font0/utf8_S.brfnt
[dvd] open Data/sysres/font1/utf8_S.brfnt
[dvd] open Data/sysres/font2/utf8_S.brfnt
[dvd] open Data/message/mes_sp.txtres
[dvd] read Data/message/mes_sp.txtres offset 0 length 238080
[dvd] open Data/layout/dialog_sp.arc
[dvd] open Data/layout/atract_4_3_sp.arc
[dvd] open Data/layout/game_sp.arc
[dvd] open Data/message/movie_jimaku_sp.csvtbl
"""


class DvdLogTests(unittest.TestCase):
    def test_real_english_run_passes_the_english_pack(self):
        dvd = language.parse_dvd_log(REAL_ENGLISH_LOG)
        self.assertEqual(language.analyse(language.ENGLISH, dvd).problems, ())
        self.assertEqual(dvd.opens[:2], ("Data/HomeButton2/homeBtn_ENG.arc", "Data/HomeButton2/SpeakerSe.arc"))
        self.assertEqual(dvd.reads, ())   # this run did not pass --log-dvd

    def test_parser_and_partial_pack_checks(self):
        synthetic = language.parse_dvd_log(SYNTHETIC_SPANISH_LOG.replace("font1/utf8_S", "font1/utf8_E"))
        problems = language.analyse(language.SPANISH, synthetic).problems
        self.assertIn("font not opened: Data/sysres/font1/utf8_S.brfnt", problems)
        self.assertIn("other language font opened: Data/sysres/font1/utf8_E.brfnt", problems)
        english_problems = language.analyse(language.ENGLISH, language.parse_dvd_log(SYNTHETIC_SPANISH_LOG)).problems
        self.assertIn("message not opened: Data/message/mes_us.txtres", english_problems)
        self.assertIn("other language message opened: Data/message/mes_sp.txtres", english_problems)
        # CRLF, an unreadable offset and an empty open are reported, never treated as records.
        dvd = language.parse_dvd_log("\r\n".join([
            "[dvd] open Data/message/mes_us.txtres", "[session] game exit=0",
            "[dvd] read Data/message/mes_us.txtres offset 32 length 64",
            "[dvd] read Data/message/mes_us.txtres offset notanumber length 4", "[dvd] open", "garbage"]) + "\r\n")
        self.assertEqual(dvd.opens, ("Data/message/mes_us.txtres",))
        self.assertEqual(dvd.reads, (language.DvdRead("Data/message/mes_us.txtres", 32, 64),))
        self.assertEqual(len(dvd.malformed), 2)
        self.assertTrue(dvd.was_read("Data/message/mes_us.txtres"))

    def test_selected_message_must_be_read_when_the_log_has_reads(self):
        # --log-dvd prints reads: a selected message that is opened but never read means the guest switched before reading it.
        log = SYNTHETIC_SPANISH_LOG.replace("[dvd] read Data/message/mes_sp.txtres offset 0 length 238080\n",
                                            "[dvd] read Data/sysres/vib.mot offset 0 length 16\n")
        self.assertIn("message opened but not read: Data/message/mes_sp.txtres",
                      language.analyse(language.SPANISH, language.parse_dvd_log(log)).problems)

    def test_live_judge_requires_the_selected_message_to_be_read(self):
        # The real English log above came from a run without --log-dvd: opens only, so the permissive check passes it.
        dvd = language.parse_dvd_log(REAL_ENGLISH_LOG)
        self.assertEqual(language.analyse(language.ENGLISH, dvd).problems, ())
        # The live judge always passes --log-dvd and must not accept opens alone as proof that the guest read the pack.
        self.assertIn("message opened but not read: Data/message/mes_us.txtres",
                      language.analyse(language.ENGLISH, dvd, require_reads=True).problems)

    def test_startup_diagnostic_is_a_cross_check_not_the_proof(self):
        log = "[language] launcher=es guest=6 message=sp font=S pack=installed\n" + SYNTHETIC_SPANISH_LOG
        info = language.parse_language_line(log)
        self.assertEqual(info, {"launcher": "es", "guest": "6", "message": "sp", "font": "S", "pack": "installed"})
        self.assertEqual(language.diagnostic_problems(language.SPANISH, info), ())
        self.assertEqual(language.diagnostic_problems(language.SPANISH, {"guest": "2", "message": "us"}),
                         ("startup diagnostic guest=2, expected 6", "startup diagnostic message=us, expected sp"))
        self.assertEqual(language.diagnostic_problems(language.SPANISH, None), ())


class ConfigTests(unittest.TestCase):
    def test_prepare_config_isolates_the_run_and_keeps_the_template(self):
        with tempfile.TemporaryDirectory() as folder:
            template = Path(folder) / "dist-config.json"
            original = json.dumps({"graphics": {"width": 1280}, "mods": {"enabled": ["x"]}})
            template.write_text(original, encoding="utf-8")
            run = Path(folder) / "run"
            path = language.prepare_config(run, "es", template=template)
            self.assertEqual(path, run / "config.json")
            written = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(written["launcher"], {"language": "es"})
            self.assertEqual(written["graphics"], {"width": 1280})
            self.assertEqual(written["mods"], {"enabled": ["x"]})
            self.assertEqual(template.read_text(encoding="utf-8"), original)   # the source config is untouched
            language.prepare_config(run, "en", template=template)
            self.assertEqual(language.config_language(path), "en")
            self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["graphics"], {"width": 1280})

    def test_prepare_config_rejects_unknown_languages_and_a_broken_launcher_object(self):
        with tempfile.TemporaryDirectory() as folder:
            run = Path(folder) / "run"
            for bad in ("xx", "ES", "", "system2"):
                with self.subTest(bad=bad), self.assertRaises(ValueError):
                    language.prepare_config(run, bad)
            broken = Path(folder) / "broken.json"
            broken.write_text('{"launcher": 5}', encoding="utf-8")
            with self.assertRaises(ValueError):
                language.prepare_config(run, "es", template=broken)
            # "system" is a valid stored value: the game resolves it at startup.
            language.prepare_config(run, "system")
            self.assertEqual(language.config_language(run / "config.json"), "system")

    def test_config_language_mirrors_the_game_validation(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "config.json"
            path.write_text("{}", encoding="utf-8")
            self.assertEqual(language.config_language(path), "system")
            path.write_text('{"launcher": {"graphics": 1}}', encoding="utf-8")
            self.assertEqual(language.config_language(path), "system")
            path.write_text('{"launcher": {"language": "pt-BR"}}', encoding="utf-8")
            self.assertEqual(language.config_language(path), "pt-BR")
            for bad in ('{"launcher": 5}', '{"launcher": {"language": null}}', '{"launcher": {"language": "bad"}}'):
                path.write_text(bad, encoding="utf-8")
                with self.subTest(bad=bad), self.assertRaises(ValueError):
                    language.config_language(path)


class PolicyTests(unittest.TestCase):
    def test_only_ordinary_play_uses_the_launcher_pack(self):
        self.assertEqual(language.expected_text("es"), language.SPANISH)
        self.assertEqual(language.expected_text("en"), language.ENGLISH)
        self.assertEqual(language.expected_text("ru"), language.ENGLISH)   # no native pack: the disc's own English
        for mode in ("netplay", "deterministic", "det-out", "demo-record", "demo-play"):
            with self.subTest(mode=mode):
                self.assertEqual(language.expected_text("es", mode), language.ENGLISH)
        with self.assertRaises(ValueError):
            language.expected_text("es", "live-switch")

    def test_mode_arguments_match_the_game_options(self):
        run = Path("build/autotest/language-x")
        demo = run / "language.demo"
        self.assertEqual(language.mode_args("ordinary", run), [])
        self.assertEqual(language.mode_args("deterministic", run), ["--deterministic"])
        self.assertEqual(language.mode_args("det-out", run),
                         ["--det-out", str(run / "trace.det"), "--det-frames", "1200"])
        self.assertEqual(language.mode_args("demo-record", run, demo=demo),
                         ["--deterministic", "--demo-record", str(demo), "--game-hz", "120"])
        self.assertEqual(language.mode_args("demo-play", run, demo=demo, stop=900),
                         ["--deterministic", "--demo-play", str(demo), "--stop-retrace", "900", "--game-hz", "120"])
        for mode in ("demo-record", "demo-play"):
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                language.mode_args(mode, run)
        with self.assertRaises(ValueError):
            language.mode_args("netplay", run)

    def test_demo_rate_is_the_demo_harness_rate(self):
        from uctest import demo
        self.assertEqual(language.DEMO_GAME_HZ, demo.DET_GAME_HZ)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
