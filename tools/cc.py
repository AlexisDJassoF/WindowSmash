"""Host toolchain wrapper: compiles generated C with clang-cl."""
import glob
import os
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor

import codec_libs   # (this folder, which every importer of cc puts on sys.path)


def _find_clang():
    """clang-cl: SSBU_CLANG if set, else Visual Studio's own (the "C++ Clang tools" component),
    newest edition first, else a standalone LLVM install, else whatever is on PATH."""
    if os.environ.get("SSBU_CLANG"):
        return os.environ["SSBU_CLANG"]
    def vs_major(path):   # the folder is a year up to 2022 (2022 = 17), a major version since (18)
        name = path.split(os.sep + "Microsoft Visual Studio" + os.sep)[1].split(os.sep)[0]
        return {"2017": 15, "2019": 16, "2022": 17}.get(name, int(name) if name.isdigit() else 0)
    found = []
    for root in (os.environ.get("ProgramFiles", r"C:\Program Files"), os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        found += sorted(glob.glob(os.path.join(root, "Microsoft Visual Studio", "*", "*", "VC", "Tools", "Llvm", "x64", "bin", "clang-cl.exe")),
                        key=vs_major, reverse=True)
    for root in (os.environ.get("ProgramFiles", r"C:\Program Files"),):
        found += glob.glob(os.path.join(root, "LLVM", "bin", "clang-cl.exe"))
    return found[0] if found else "clang-cl"


CLANG = _find_clang()
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNTIME = os.path.join(ROOT, "runtime")
# Guest FP semantics need IEEE-exact arithmetic: no contraction, no fast-math. (BASE_CFLAGS is
# also what the codec libraries are compiled with: same instruction set, same C runtime.)
BASE_CFLAGS = ["/nologo", "/arch:AVX2", "-mfma", "-mf16c", "-fno-strict-aliasing", "/clang:-ffp-contract=off", "-w"]
CFLAGS = [*BASE_CFLAGS, "/I", RUNTIME]

# The audio and video codecs: libopus for the hwopus service (music) and libvpx for the video
# decode engine's VP9 (movies). Normally built from the sources under third_party/ by
# tools/codec_libs.py when link() runs. A developer can supply libraries instead: SSBU_OPUS_LIBS
# is a semicolon-separated list of Windows x64 .lib files, SSBU_VPX_DIR a folder with include/
# and lib/Win64/Release/libvpx.lib.
OPUS_LIBS = [p for p in os.environ.get("SSBU_OPUS_LIBS", "").split(";") if p]
if not all(os.path.exists(p) for p in OPUS_LIBS):
    OPUS_LIBS = []
VPX_DIR = os.environ.get("SSBU_VPX_DIR", "")
VPX_INCLUDE = os.path.join(VPX_DIR, "include")
VPX_LIBS = [os.path.join(VPX_DIR, r"lib\Win64\Release\libvpx.lib")] if VPX_DIR else []
if not all(os.path.exists(p) for p in VPX_LIBS) or not os.path.exists(os.path.join(VPX_INCLUDE, "vpx", "vpx_decoder.h")):
    VPX_LIBS = []
# (supplied libraries were built against the DLL C runtime; this program uses the static one)
EXTERNAL_LIBS = OPUS_LIBS + VPX_LIBS
BUILT_CODECS = []   # the libraries codec_libs builds for this link, from third_party/
if not OPUS_LIBS and codec_libs.have_opus():
    OPUS_LIBS, BUILT_CODECS = [codec_libs.OPUS_LIB], BUILT_CODECS + ["opus"]
if not VPX_LIBS and codec_libs.have_vpx():
    VPX_LIBS, VPX_INCLUDE, BUILT_CODECS = [codec_libs.VPX_LIB], codec_libs.VPX, BUILT_CODECS + ["libvpx"]
if OPUS_LIBS:
    CFLAGS.append("/DSSBU_HAVE_OPUS")
if VPX_LIBS:
    CFLAGS += ["/DSSBU_HAVE_VPX", "/I", VPX_INCLUDE]
# SDL3 for controllers (zlib licence): the runtime loads SDL3.dll at run time, so the build needs only the headers,
# and the link step copies the DLL beside the exe. SSBU_SDL3_DIR overrides where (include/, lib/x64/SDL3.dll).
SDL3_DIR = os.environ.get("SSBU_SDL3_DIR", os.path.join(ROOT, "deps", "SDL3"))
SDL3_DLL = os.path.join(SDL3_DIR, r"lib\x64\SDL3.dll")
if os.path.exists(os.path.join(SDL3_DIR, "include", "SDL3", "SDL_gamepad.h")) and os.path.exists(SDL3_DLL):
    CFLAGS += ["/DSSBU_HAVE_SDL3", "/I", os.path.join(SDL3_DIR, "include")]
else:
    SDL3_DLL = None
EXTRA_LIBS = OPUS_LIBS + VPX_LIBS + (["/NODEFAULTLIB:msvcrt.lib"] if EXTERNAL_LIBS else [])


def compile_one(src, obj, opt="/O2"):
    r = subprocess.run([CLANG, "/c", opt, *CFLAGS, src, "/Fo" + obj], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"compile failed: {src}\n{(r.stdout + r.stderr)[:4000]}")
    return obj


def compile_many(sources, jobs=16, opt="/O2"):
    """Compile each source next to itself; returns the object paths."""
    with ThreadPoolExecutor(jobs) as pool:
        return list(pool.map(lambda s: compile_one(s, os.path.splitext(s)[0] + ".obj", opt), sources))


def compile_each(jobs_list, jobs=16, started=None, finished=None):
    """Compile (src, obj, opt) triples in parallel, in order; returns [(src, error text)] for
    failures. started(job) and finished(job, seconds), if given, are called (from the worker
    threads) as each one starts and ends."""
    def one(job):
        src, obj, opt = job
        if started:
            started(job)
        t0 = time.time()
        r = subprocess.run([CLANG, "/c", opt, *CFLAGS, src, "/Fo" + obj], capture_output=True, text=True)
        if finished:
            finished(job, time.time() - t0)
        return (src, r.stdout + r.stderr) if r.returncode else None
    with ThreadPoolExecutor(jobs) as pool:
        return [f for f in pool.map(one, jobs_list) if f]


def link(objs, exe, options=()):
    """Link objects into an executable; `options` go to the linker. The codec libraries built
    from third_party/ are made first if they are missing or stale (a few seconds)."""
    if BUILT_CODECS:
        codec_libs.build(BUILT_CODECS, log=lambda m: print(m, flush=True))
    rsp = exe + ".rsp"
    with open(rsp, "w") as f:
        f.write("\n".join(f'"{o}"' for o in objs))
    lld = ["-fuse-ld=lld"] if os.path.exists(os.path.join(os.path.dirname(CLANG), "lld-link.exe")) else []
    r = subprocess.run([CLANG, "/nologo", *lld, "@" + rsp, "/Fe" + exe, "/link", "/HIGHENTROPYVA:NO", "/MAP:" + exe[:-4] + ".map", *options, "winmm.lib", "user32.lib", "gdi32.lib", "d3d11.lib", "dxgi.lib", "d3dcompiler.lib", "dxguid.lib", "ws2_32.lib", "ole32.lib", "oleaut32.lib", *EXTRA_LIBS],
                       capture_output=True, text=True)
    if r.returncode:
        lines = (r.stdout + r.stderr).splitlines()
        errors = [l for l in lines if "error" in l.lower()] or lines   # (the third-party libraries' CRT warnings drown them)
        raise RuntimeError("link failed\n" + "\n".join(errors)[:4000])
    return exe
