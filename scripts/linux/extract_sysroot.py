"""Extract the Steam Runtime sniper SDK sysroot on Windows without symlink rights.

Only what a cross build needs is kept (headers, x86_64 libraries, gcc support files). Symlinks to files become copies, symlinks to
directories become NTFS junctions (no administrator needed); what cannot be resolved is listed in links.txt.

  python scripts/linux/extract_sysroot.py <archive.tar.gz> <out-dir>

The archive is com.valvesoftware.SteamRuntime.Sdk-amd64,i386-sniper-sysroot.tar.gz from
https://repo.steampowered.com/steamrt3/images/latest-public-stable/ (1.2 GB, about 6 GB extracted).
"""
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

SKIP = ("usr/lib/python", "usr/lib/debug", "usr/lib/i386", "usr/lib32", "usr/lib/git-core", "usr/lib/jvm", "usr/share", "usr/libexec",
        "usr/local", "usr/src", "var", "etc", "root", "home", "opt", "srv", "tmp", "boot", "proc", "sys", "dev", "run", "mnt", "media",
        "usr/lib/ruby", "usr/lib/perl", "usr/lib/valgrind", "usr/lib/ssl", "usr/lib/gnupg", "usr/lib/apt", "usr/lib/dpkg", "usr/sbin", "sbin",
        "bin", "usr/bin", "lib/i386", "lib32", "libx32", "usr/libx32", "usr/lib/x86_64-linux-gnu/perl", "usr/lib/x86_64-linux-gnu/gconv",
        "usr/lib/x86_64-linux-gnu/krb5", "usr/lib/x86_64-linux-gnu/sasl2", "usr/lib/x86_64-linux-gnu/girepository-1.0")


def wanted(name):
    name = name.lstrip("./")
    if not name:
        return False
    return not any(name == s or name.startswith(s + "/") for s in SKIP)


def long(path):
    return "\\\\?\\" + str(path.resolve()) if os.name == "nt" else str(path)


def make_junction(link, target):
    if os.name == "nt":
        subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)], check=True, stdout=subprocess.DEVNULL)
    else:
        os.symlink(target, link)


def main(archive, out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    links, hard = [], []
    kept = 0
    with tarfile.open(archive, "r|gz") as tar:
        for m in tar:
            name = m.name.lstrip("./")
            if not wanted(name):
                continue
            dest = out / name
            if m.isdir():
                os.makedirs(long(dest), exist_ok=True)
            elif m.issym():
                links.append((name, m.linkname))
            elif m.islnk():
                hard.append((name, m.linkname.lstrip("./")))
            elif m.isreg():
                os.makedirs(long(dest.parent), exist_ok=True)
                with open(long(dest), "wb") as f:
                    shutil.copyfileobj(tar.extractfile(m), f)
                kept += 1
    for name, target in hard:
        src = out / target
        if src.is_file():
            os.makedirs(long((out / name).parent), exist_ok=True)
            shutil.copyfile(long(src), long(out / name))
    dir_links, pending = [], links
    for _ in range(6):   # a link can point at another link: repeat until nothing more resolves
        nxt = []
        for name, target in pending:
            base = out / name
            src = Path(os.path.normpath((out / target.lstrip("/")) if target.startswith("/") else (base.parent / target)))
            if src.is_file():
                os.makedirs(long(base.parent), exist_ok=True)
                try:
                    shutil.copyfile(long(src), long(base))
                except shutil.SameFileError:
                    pass   # the link differs from its target only by letter case (Fl_File_Chooser.H): one file on this file system
            elif src.is_dir():
                dir_links.append((name, src))
            else:
                nxt.append((name, target))
        if len(nxt) == len(pending):
            break
        pending = nxt
    made = 0
    for name, src in dir_links:
        link = out / name
        if not link.exists():
            os.makedirs(long(link.parent), exist_ok=True)
            make_junction(link, src)
            made += 1
    with open(out / "links.txt", "w") as f:
        for name, target in pending:
            f.write(f"unresolved {name} -> {target}\n")
    print(f"files {kept}, hard links {len(hard)}, junctions {made}, unresolved {len(pending)} (links.txt)")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
