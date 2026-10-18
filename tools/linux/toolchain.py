"""Cross toolchain for the Linux build: the Visual Studio clang, targeting x86_64-linux-gnu.

Everything third-party lives outside the repository, in linux-toolchain/ next to this checkout
(SSBU_LINUX_TOOLCHAIN overrides; README-LINUX.md says where each part comes from):
  sysroot/        Ubuntu 22.04 packages (glibc 2.35, libgcc, libstdc++, SDL2, libopus, libvpx)
  dxvk-native/    DXVK's native Linux libraries (Direct3D 11 on Vulkan)
  dxvk-headers/   their headers (DXVK's include/native and mingw-directx-headers)
  src/vkd3d-*/    vkd3d source; build-vkd3d/ holds its generated parsers and libraries
Linking against glibc 2.35 keeps the program runnable on Ubuntu 22.04 and anything newer."""
import glob
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
RUNTIME = os.path.join(REPO, "runtime")
TOOLCHAIN = os.environ.get("SSBU_LINUX_TOOLCHAIN") or os.path.join(os.path.dirname(REPO), "linux-toolchain")
SYSROOT = os.path.join(TOOLCHAIN, "sysroot")
DXVK_LIBS = os.path.join(TOOLCHAIN, "dxvk-native", "usr", "lib")
DXVK_INCLUDE = os.path.join(TOOLCHAIN, "dxvk-headers", "include", "native")
VKD3D_SRC = sorted(glob.glob(os.path.join(TOOLCHAIN, "src", "vkd3d-*")))[-1:] or [""]
VKD3D_SRC = VKD3D_SRC[0]
VKD3D_BUILD = os.path.join(TOOLCHAIN, "build-vkd3d")

# The game tree (dump/, build/, save/) a Linux build is made from unless --port names another:
# the 13.0.5 update's folder (tools/update.py --tree v13.0.5) once it is built, otherwise the
# port's own tree. That is this checkout when it has game data of its own (dump/), otherwise a
# native-port checkout next to it (the Linux work in a worktree of its own). The build's folder
# and the package's dump/ record theirs in port.txt, so the two never mix trees.
PORT_DIR = REPO if os.path.isdir(os.path.join(REPO, "dump")) else os.path.join(os.path.dirname(REPO), "native-port")


def default_port():
    tree = os.path.join(PORT_DIR, "v13.0.5")
    return tree if os.path.isdir(os.path.join(tree, "build")) else PORT_DIR


def same_path(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def read_port_stamp(folder):
    try:
        with open(os.path.join(folder, "port.txt")) as f:
            return f.read().strip() or None
    except OSError:
        return None


def write_port_stamp(folder, port):
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "port.txt"), "w", newline="\n") as f:
        f.write(port + "\n")


def _find_llvm():
    if os.environ.get("SSBU_LLVM_BIN"):
        return os.environ["SSBU_LLVM_BIN"]
    roots = [os.environ.get("ProgramFiles", r"C:\Program Files"), os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")]
    found = []
    for root in roots:
        found += sorted(glob.glob(os.path.join(root, "Microsoft Visual Studio", "*", "*", "VC", "Tools", "Llvm", "x64", "bin")), reverse=True)
    found += [os.path.join(roots[0], "LLVM", "bin")]
    for d in found:
        if os.path.exists(os.path.join(d, "clang.exe")) and os.path.exists(os.path.join(d, "ld.lld.exe")):
            return d
    raise SystemExit("no clang + ld.lld found (set SSBU_LLVM_BIN)")


LLVM = _find_llvm()
CLANG = os.path.join(LLVM, "clang.exe")
LLVM_AR = os.path.join(LLVM, "llvm-ar.exe")
TARGET = ["--target=x86_64-linux-gnu", "--sysroot=" + SYSROOT]
# Guest FP semantics need IEEE-exact arithmetic: no contraction, no fast-math (as on Windows).
# -fno-pic: the program is linked at a fixed address (non-PIE); its code is far over the size
# where PIC's extra indirections are free.
ARCH = ["-mavx2", "-mfma", "-mf16c", "-mbmi", "-mbmi2", "-mlzcnt", "-mpopcnt"]
CFLAGS = TARGET + ARCH + ["-fno-strict-aliasing", "-ffp-contract=off", "-fno-pic", "-fno-plt", "-w",
                          "-fasynchronous-unwind-tables", "-I", RUNTIME]


# (below normal priority: a build here should not get in the way of anything else on the machine)
_PRIORITY = getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, creationflags=_PRIORITY)
    return r.returncode, r.stdout + r.stderr


def compile_each(jobs_list, jobs=16, extra=()):
    """Compile (src, obj, opt[, flags]) jobs in parallel; returns [(src, error text)] for failures."""
    def one(job):
        src, obj, opt = job[:3]
        flags = job[3] if len(job) > 3 else []
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        lang = ["-x", "c++", "-std=gnu++20", "-fexceptions"] if src.endswith((".cpp", ".cc")) or "c++" in flags else []
        flags = [f for f in flags if f != "c++"]
        code, out = run([CLANG, "-c", opt, *CFLAGS, *lang, *extra, *flags, src, "-o", obj])
        return (src, out) if code else None
    with ThreadPoolExecutor(jobs) as pool:
        return [f for f in pool.map(one, jobs_list) if f]
