#!/usr/bin/env python3
"""Rebuild the Linux program after a change to the runtime, without the game's generated C.

Usage: relink_runtime.py [--out <build-linux dir>] [--jobs N] [--install [<package dir>]]

build_linux.py needs the C tools/recomp.py generates (<port>/build/), even with --runtime-only.
Once the game is compiled, its objects in <out>/{main,subsdk0,sdk,nro/*} are all the link needs:
this compiles again the runtime files (runtime/*.c and runtime/linux) whose source or a header is
newer than their object, with build_linux.py's flags, and links <out>/ssbu from those objects and
the game's. The generated tables (<out>/gen: HLE wrappers, NRO list) are kept as they were built.
--install then copies ssbu and ssbu.syms into the package run.sh plays from (default
dist-linux/ssbu-linux); a package made with package_linux.py --link already points at the build."""
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
INSTALL = "--install" in sys.argv
PACKAGE = os.path.join(REPO, "dist-linux", "ssbu-linux")
if INSTALL:
    i = sys.argv.index("--install")
    if i + 1 < len(sys.argv) and not sys.argv[i + 1].startswith("--"):
        PACKAGE = os.path.abspath(sys.argv.pop(i + 1))
    sys.argv.pop(i)
sys.argv += ["--port", REPO]   # (build_linux reads its options at import: the port is not used here)
sys.path.insert(0, HERE)
import build_linux as B   # noqa: E402


def built_modules():
    """(name, a source path of the shape build_linux expects, output directory) for each module compiled in <out>."""
    found = [(n, os.path.join("build", n), os.path.join(B.OUT, n)) for n in ("main", "subsdk0", "sdk") if os.path.isdir(os.path.join(B.OUT, n))]
    nro = os.path.join(B.OUT, "nro")
    if os.path.isdir(nro):
        found += [(n, os.path.join("build", "nro", n), os.path.join(nro, n)) for n in sorted(os.listdir(nro))]
    return found


def runtime_jobs():
    """build_linux.runtime_jobs's compiles of the runtime's own files, without its generated tables."""
    R, rt_out = B.RUNTIME, os.path.join(B.OUT, "rt")
    headers = [os.path.join(R, n) for n in os.listdir(R) if n.endswith(".h")] + \
              [os.path.join(R, "linux", n) for n in os.listdir(os.path.join(R, "linux")) if n.endswith(".h")] + \
              [os.path.join(R, "linux", "include", "windows.h")]
    sources = [os.path.join(R, n) for n in sorted(os.listdir(R))
               if re.fullmatch(r"(host|kernel|gpu\w*|hle\w*|ipc\w*|net\w*|pad_menu|debug_watchdog)\.c", n)]
    sources += [os.path.join(R, "linux", n) for n in sorted(os.listdir(os.path.join(R, "linux"))) if n.endswith((".c", ".cpp"))]
    jobs = []
    for src in sources:
        obj = os.path.join(rt_out, os.path.basename(src).rsplit(".", 1)[0] + ".o")
        renames = ["-include", os.path.join(R, "linux", "gpu_d3d11_names.h")] if os.path.basename(src) == "gpu_d3d11.c" else []
        if B.needs(obj, src, *headers):
            jobs.append((src, obj, "-O2", B.RUNTIME_INCLUDES + B.RUNTIME_DEFINES + renames))
    return jobs


def install():
    exe = os.path.join(PACKAGE, "ssbu")
    if not os.path.isdir(PACKAGE):
        raise SystemExit(f"no package in {PACKAGE} (tools/linux/package_linux.py makes it)")
    if os.path.islink(exe):
        print(f"{exe} is a link to the build: nothing to copy")
        return
    for name in ("ssbu", "ssbu.syms"):   # (a new file put in place: a game running keeps the old one)
        src = os.path.join(B.OUT, name)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(PACKAGE, name + ".new"))
            os.replace(os.path.join(PACKAGE, name + ".new"), os.path.join(PACKAGE, name))
    print(f"copied ssbu and ssbu.syms into {PACKAGE}")


def main():
    B.modules = built_modules
    if not B.modules():
        raise SystemExit(f"no compiled game in {B.OUT}: build it first (tools/build_game.py)")
    jobs = runtime_jobs()
    if not B.compile_all(jobs, "runtime") or not B.link():
        return False
    if INSTALL:
        install()
    return True


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
