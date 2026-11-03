"""Runs slangc (the command is the arguments) and drops the DirectX 11 compiler (fxc) warnings that were checked and are harmless.

A new warning, or one of these codes in another shader, still prints. Checked 2026-11-03 against the HLSL Slang writes:
  tev.slang X4000       fxc reports the return value of colorSource, alphaSource and evaluateTev as uninitialized; every path returns a value (false positive of its flow analysis)
  smoke.slang X3578     vertexMain of the shadow variant (-DSHADOW) writes the position only, by design: that pass has no fragment stage
  bloom.slang X3571     pow() of a saturated colour plus the glow, never negative
  hdr.slang, motion_blur.slang X3556   integer divide by a positive size: a speed hint only
"""
import re
import subprocess
import sys

HARMLESS = {("tev", "4000"), ("smoke", "3578"), ("bloom", "3571"), ("hdr", "3556"), ("motion_blur", "3556")}
WARNING = re.compile(r"^fxc \d+\.\d+: .*[/\\](\w+)\.slang\(\d+\): warning X(\d+):")


def filtered(text):
    def harmless(line):
        found = WARNING.match(line)
        return bool(found) and (found.group(1), found.group(2)) in HARMLESS
    return "".join(line for line in text.splitlines(keepends=True) if not harmless(line))


def main(command):
    done = subprocess.run(command, capture_output=True, text=True, errors="replace")
    sys.stdout.write(filtered(done.stdout))
    sys.stderr.write(filtered(done.stderr))
    return done.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
