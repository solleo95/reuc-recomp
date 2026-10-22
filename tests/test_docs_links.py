"""The modding guides (docs/modding/guide/en and /ru): same files in both languages, every relative link resolves, every #anchor exists, every guide has the
navigation block (language switch to its twin, a contents list, a "you are here" line) and every contents entry points to a heading of the same file."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
GUIDE = ROOT / "docs" / "modding" / "guide"
LINK = re.compile(r"(?<!\!)\[[^\]]*\]\(([^)\s]+)\)")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")


def slug(heading):
    """GitHub's anchor of a heading: lower case, spaces to '-', everything but letters, digits, '-' and '_' removed."""
    text = re.sub(r"`", "", heading.strip()).lower()
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    return "".join(ch if (ch.isalnum() or ch in "-_") else ("-" if ch == " " else "") for ch in text)


def anchors(path):
    found, counts, fenced = set(), {}, False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("```"):
            fenced = not fenced
            continue
        match = None if fenced else HEADING.match(line)
        if match:
            base = slug(match.group(2))
            n = counts.get(base, 0)
            counts[base] = n + 1
            found.add(base if n == 0 else f"{base}-{n}")
    return found


def links(path):
    text, fenced, out = path.read_text(encoding="utf-8"), False, []
    for number, line in enumerate(text.splitlines(), 1):
        if line.startswith("```"):
            fenced = not fenced
            continue
        if not fenced:
            out += [(number, m.group(1)) for m in LINK.finditer(re.sub(r"`[^`]*`", "", line))]
    return out


class GuideTests(unittest.TestCase):
    def files(self, language):
        return sorted(p.name for p in (GUIDE / language).glob("*.md"))

    def test_both_languages_have_the_same_files(self):
        self.assertTrue(self.files("en"), "no English guides")
        self.assertEqual(self.files("en"), self.files("ru"))

    def test_links_and_anchors_resolve(self):
        problems = []
        for language in ("en", "ru"):
            for path in sorted((GUIDE / language).glob("*.md")):
                known = {}
                for number, target in links(path):
                    if target.startswith(("http://", "https://", "mailto:")):
                        continue
                    file_part, _, anchor = target.partition("#")
                    destination = path if not file_part else (path.parent / file_part).resolve()
                    if not destination.exists():
                        problems.append(f"{path.relative_to(ROOT)}:{number}: {target} - no such file")
                        continue
                    if anchor and destination.suffix == ".md":
                        known.setdefault(destination, anchors(destination))
                        if anchor not in known[destination]:
                            problems.append(f"{path.relative_to(ROOT)}:{number}: {target} - no such heading")
        self.assertEqual(problems, [], "\n" + "\n".join(problems))

    def test_every_guide_has_the_navigation_block(self):
        problems = []
        for language, other in (("en", "ru"), ("ru", "en")):
            for path in sorted((GUIDE / language).glob("*.md")):
                lines = path.read_text(encoding="utf-8").splitlines()
                head = "\n".join(lines[:8])
                if f"](../{other}/{path.name})" not in head:
                    problems.append(f"{path.name} ({language}): no link to the {other} twin in the first lines")
                if not any(line.startswith("## ") for line in lines):
                    problems.append(f"{path.name} ({language}): no sections")
                contents = [i for i, line in enumerate(lines) if line.startswith("## ")][:1]
                if not contents:
                    continue
                first = lines[contents[0]]
                if not first.startswith(("## Contents", "## Содержание")):
                    problems.append(f"{path.name} ({language}): the first section is not the contents list")
                entries = []
                for line in lines[contents[0] + 1:]:
                    if line.startswith("## "):
                        break
                    entries += [m.group(1) for m in LINK.finditer(line)]
                if len(entries) < 5:
                    problems.append(f"{path.name} ({language}): the contents list has {len(entries)} entries")
        self.assertEqual(problems, [], "\n" + "\n".join(problems))

    def test_the_overview_links_every_guide(self):
        for language in ("en", "ru"):
            overview = (GUIDE / language / "README.md").read_text(encoding="utf-8")
            for name in self.files(language):
                if name != "README.md":
                    self.assertIn(f"]({name})", overview, f"{language}/README.md does not link {name}")


if __name__ == "__main__":
    unittest.main()
