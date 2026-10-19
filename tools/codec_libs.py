#!/usr/bin/env python3
"""The audio and video codecs, built from source: libopus (the game's music) and libvpx's VP9
decoder (its movies).

Usage:
  codec_libs.py [--jobs N] [--force]     build build/codecs/opus.lib and libvpx.lib if stale
  codec_libs.py --status                 say what is there

Their sources live under third_party/opus and third_party/libvpx (third_party/README.md says
which upstream versions and how they were taken). `build()` compiles them with clang-cl into
build/codecs/ - some 200 small C files, a few seconds - and skips a library that is newer than
its sources. tools/cc.py adds the libraries and their defines to every link whose sources are
there, so music and movies work without the developer setting SSBU_OPUS_LIBS or SSBU_VPX_DIR;
those two still win when they are set.

Why plain C for libvpx: its x86 assembly needs nasm, one more tool to install, and the
intrinsics-only files depend on symbols from that assembly. Measured on the game's 1280x720
title movie (1,069 frames, four decoder threads): about 570 frames a second in plain C against
1,100 with the assembly - either far above the 30 the game shows. libopus keeps its SSE and
AVX2 intrinsics, which are plain C for a compiler told /arch:AVX2 (the whole program is).
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)   # (for cc, also when this runs on its own under a Python that does not add it: recomp.py)
ROOT = os.path.dirname(TOOLS)
THIRD_PARTY = os.path.join(ROOT, "third_party")
OPUS = os.path.join(THIRD_PARTY, "opus")
VPX = os.path.join(THIRD_PARTY, "libvpx")
OUT = os.path.join(ROOT, "build", "codecs")
OPUS_LIB = os.path.join(OUT, "opus.lib")
VPX_LIB = os.path.join(OUT, "libvpx.lib")

# The source folders of each library, as its own build lists them for a float, x86-64 build
# without run-time CPU detection (opus) and a VP9-decoder-only build in plain C (libvpx).
OPUS_FOLDERS = ("celt", "celt/x86", "silk", "silk/x86", "silk/float", "silk/float/x86", "src")
OPUS_INCLUDES = ("include", "celt", "silk", "silk/float", "src", "")
# opus's build-system defines: no config.h; alloca for its scratch buffers (no C99 VLAs with
# clang-cl); the x86 intrinsics present and presumed (the program needs AVX2 anyway, so there is
# nothing to detect at run time)
OPUS_DEFINES = ("OPUS_BUILD", "USE_ALLOCA", "HAVE_LRINT", "HAVE_LRINTF", 'PACKAGE_VERSION="1.5.2"',
                "OPUS_X86_MAY_HAVE_SSE", "OPUS_X86_MAY_HAVE_SSE2", "OPUS_X86_MAY_HAVE_SSE4_1", "OPUS_X86_MAY_HAVE_AVX2",
                "OPUS_X86_PRESUME_SSE", "OPUS_X86_PRESUME_SSE2", "OPUS_X86_PRESUME_SSE4_1", "OPUS_X86_PRESUME_AVX2")
VPX_FOLDERS = ("vp9", "vp9/common", "vp9/decoder", "vpx/src", "vpx_dsp", "vpx_mem", "vpx_ports",
               "vpx_scale", "vpx_scale/generic", "vpx_util")
VPX_INCLUDES = ("config", "")   # config/: the headers libvpx's configure and make generate
VPX_DEFINES = ("_CRT_SECURE_NO_WARNINGS",)


def have_opus():
    return os.path.isfile(os.path.join(OPUS, "src", "opus_decoder.c"))


def have_vpx():
    return os.path.isfile(os.path.join(VPX, "vp9", "vp9_dx_iface.c")) and \
        os.path.isfile(os.path.join(VPX, "config", "vpx_config.h"))


def sources(root, folders):
    """The .c files of the given folders under `root` (not their subfolders), sorted."""
    out = []
    for folder in folders:
        d = os.path.join(root, folder) if folder else root
        if os.path.isdir(d):
            out += sorted(os.path.join(d, n) for n in os.listdir(d) if n.endswith(".c"))
    return out


def inputs(root, folders, includes):
    """Everything a library is built from: its sources, the headers next to them and in its
    include folders, and this file (its flags)."""
    files = sources(root, folders)
    for folder in set(folders) | set(includes):
        d = os.path.join(root, folder) if folder else root
        if os.path.isdir(d):
            files += [os.path.join(d, n) for n in os.listdir(d) if n.endswith(".h")]
    return files + [os.path.abspath(__file__)]


def is_current(lib, root, folders, includes):
    """The library is there and newer than everything it is built from."""
    if not os.path.isfile(lib):
        return False
    return os.path.getmtime(lib) >= max(os.path.getmtime(p) for p in inputs(root, folders, includes))


def archiver(clang):
    """llvm-lib next to clang-cl (Visual Studio's Clang tools and LLVM both ship it), or lib.exe."""
    candidate = os.path.join(os.path.dirname(clang), "llvm-lib.exe")
    if os.path.isfile(candidate):
        return [candidate]
    for name in ("llvm-lib", "lib"):
        if shutil.which(name):
            return [shutil.which(name)]
    return None


def build_lib(name, lib, root, folders, includes, defines, jobs, log):
    import cc
    objdir = os.path.join(OUT, name)
    os.makedirs(objdir, exist_ok=True)
    flags = [*cc.BASE_CFLAGS, "/O2"]
    for inc in includes:
        flags += ["/I", os.path.join(root, inc) if inc else root]
    flags += ["/D" + d for d in defines]
    srcs = sources(root, folders)
    t0 = time.time()

    def one(src):
        obj = os.path.join(objdir, os.path.relpath(src, root).replace(os.sep, "_")[:-2] + ".obj")
        r = subprocess.run([cc.CLANG, "/c", *flags, src, "/Fo" + obj], capture_output=True, text=True)
        return (src, r.stdout + r.stderr) if r.returncode else obj

    with ThreadPoolExecutor(jobs) as pool:
        results = list(pool.map(one, srcs))
    failures = [r for r in results if isinstance(r, tuple)]
    for src, err in failures[:3]:
        log(f"FAILED {os.path.relpath(src, root)}:\n{err[:2000]}")
    if failures:
        raise RuntimeError(f"{name}: {len(failures)} of {len(srcs)} files did not compile")
    objs = [r for r in results if not isinstance(r, tuple)]
    ar = archiver(cc.CLANG)
    if not ar:
        raise RuntimeError("no llvm-lib.exe or lib.exe to make a static library with")
    rsp = lib + ".rsp"
    with open(rsp, "w") as f:
        f.write("\n".join(f'"{o}"' for o in objs))
    partial = lib + ".partial"
    r = subprocess.run([*ar, "/nologo", "/OUT:" + partial, "@" + rsp], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"{name}: making the library failed\n{(r.stdout + r.stderr)[:2000]}")
    os.replace(partial, lib)
    log(f"  {os.path.relpath(lib, ROOT)}: {len(objs)} files in {time.time() - t0:.1f}s")


LIBRARIES = {
    "opus": (OPUS_LIB, OPUS, OPUS_FOLDERS, OPUS_INCLUDES, OPUS_DEFINES, have_opus),
    "libvpx": (VPX_LIB, VPX, VPX_FOLDERS, VPX_INCLUDES, VPX_DEFINES, have_vpx),
}


def build(which=("opus", "libvpx"), jobs=16, force=False, log=print):
    """Build the named libraries whose sources are there and whose library is stale. Returns
    the libraries that are there afterwards."""
    libs = []
    for name in which:
        lib, root, folders, includes, defines, have = LIBRARIES[name]
        if not have():
            continue
        if force or not is_current(lib, root, folders, includes):
            log(f"  building {name} from {os.path.relpath(root, ROOT)}")
            build_lib(name, lib, root, folders, includes, defines, jobs, log)
        libs.append(lib)
    return libs


def main():
    ap = argparse.ArgumentParser(description="Build the codec libraries from third_party/.")
    ap.add_argument("--jobs", type=int, default=16)
    ap.add_argument("--force", action="store_true", help="build even when the libraries are up to date")
    ap.add_argument("--status", action="store_true", help="only say what is there")
    a = ap.parse_args()
    for name, (lib, root, folders, includes, defines, have) in LIBRARIES.items():
        state = "no sources" if not have() else "up to date" if is_current(lib, root, folders, includes) else "to build"
        print(f"{name}: {len(sources(root, folders))} sources, {os.path.relpath(lib, ROOT)} {state}")
    if a.status:
        return 0
    try:
        build(jobs=a.jobs, force=a.force)
    except RuntimeError as e:
        print(f"ERROR: {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
