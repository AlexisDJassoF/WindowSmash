"""Cross toolchain for the Linux build: the Visual Studio clang, targeting x86_64-linux-gnu.

Everything third-party lives outside the repository, in linux-toolchain/ next to this checkout
(SSBU_LINUX_TOOLCHAIN overrides; README-LINUX.md says where each part comes from):
  sysroot/        Ubuntu 22.04 packages (glibc 2.35, libgcc, libstdc++ and its development link, SDL2, libopus, libvpx)
  dxvk-native/    DXVK's native Linux libraries (Direct3D 11 on Vulkan)
  dxvk-headers/   their headers (DXVK's include/native and mingw-directx-headers)
  src/vkd3d-*/    vkd3d source; build-vkd3d/ holds its generated parsers and libraries
Linking against glibc 2.35 keeps the program runnable on Ubuntu 22.04 and anything newer."""
import glob
import os
import re
import shutil
import subprocess
import sys
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
# (the Vulkan renderer's headers - runtime/linux/gpu_vulkan*.c; Vulkan itself is loaded at run time)
VULKAN_INCLUDE = (sorted(glob.glob(os.path.join(TOOLCHAIN, "src", "Vulkan-Headers-*", "include")))[-1:] or [""])[0]
# (SPIR-V's opcodes and enums: spirv/unified1/spirv.h, for the Maxwell-to-SPIR-V translator, runtime/linux/gpu_spirv*.c)
SPIRV_INCLUDE = (sorted(glob.glob(os.path.join(TOOLCHAIN, "src", "SPIRV-Headers-*", "include")))[-1:] or [""])[0]

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


LINUX_PACKAGES = {"clang": "clang", "ld.lld": "lld", "llvm-ar": "llvm", "llvm-nm": "llvm"}


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


def find_linux_llvm(env=os.environ, which=shutil.which, isfile=os.path.isfile, find=glob.glob):
    """On a Linux host: {tool: path} for clang, ld.lld, llvm-ar and llvm-nm, all of one LLVM.
    SSBU_LLVM_BIN names their folder; else the unsuffixed names on PATH (Ubuntu's clang, lld and
    llvm packages); else the newest /usr/lib/llvm-NN/bin that has all four; else the newest
    version NN whose clang-NN, ld.lld-NN, llvm-ar-NN and llvm-nm-NN are all on PATH."""
    if env.get("SSBU_LLVM_BIN"):
        folder = env["SSBU_LLVM_BIN"]
        tools = {t: os.path.join(folder, t) for t in LINUX_PACKAGES}
        missing = [t for t, p in tools.items() if not isfile(p)]
        if missing:
            raise SystemExit(f"SSBU_LLVM_BIN={folder} has no {', '.join(missing)}")
        return tools
    plain = {t: which(t) for t in LINUX_PACKAGES}
    if all(plain.values()):
        return plain
    def version(folder):   # /usr/lib/llvm-NN/bin -> NN
        m = re.search(r"llvm-(\d+)/bin$", folder)
        return int(m.group(1)) if m else 0
    for folder in sorted(find("/usr/lib/llvm-*/bin"), key=version, reverse=True):
        tools = {t: os.path.join(folder, t) for t in LINUX_PACKAGES}
        if all(isfile(p) for p in tools.values()):
            return tools
    versions = set()
    for folder in env.get("PATH", "").split(os.pathsep):
        for path in find(os.path.join(folder, "clang-[0-9]*")):
            m = re.fullmatch(r"clang-(\d+)", os.path.basename(path))
            if m:
                versions.add(int(m.group(1)))
    for v in sorted(versions, reverse=True):
        tools = {t: which(f"{t}-{v}") for t in LINUX_PACKAGES}
        if all(tools.values()):
            return tools
    missing = sorted({LINUX_PACKAGES[t] for t, p in plain.items() if not p})
    raise SystemExit("the LLVM tools are missing (clang, ld.lld, llvm-ar, llvm-nm): install them with\n"
                     f"  sudo apt install {' '.join(missing)}\nor name their folder in SSBU_LLVM_BIN")


HOST_LINUX = sys.platform.startswith("linux")
_tools = None


def tools():
    """LLVM (their folder), CLANG, LLVM_AR, LLVM_NM and LINKER (clang's flags naming the linker),
    found the first time one is asked for - the module's attributes of those names are these - so
    that what only reads the layout (package_linux.py) needs no compiler."""
    global _tools
    if _tools is None:
        if HOST_LINUX:
            found = find_linux_llvm()
            _tools = {"LLVM": os.path.dirname(found["clang"]), "CLANG": found["clang"], "LLVM_AR": found["llvm-ar"],
                      "LLVM_NM": found["llvm-nm"],
                      # (an absolute linker: -fuse-ld=lld would not find a version-suffixed ld.lld-NN)
                      "LINKER": ["--ld-path=" + found["ld.lld"]]}
        else:
            llvm = _find_llvm()
            _tools = {"LLVM": llvm, "CLANG": os.path.join(llvm, "clang.exe"), "LLVM_AR": os.path.join(llvm, "llvm-ar.exe"),
                      "LLVM_NM": os.path.join(llvm, "llvm-nm.exe"), "LINKER": ["-fuse-ld=lld"]}
    return _tools


def __getattr__(name):
    if name in ("LLVM", "CLANG", "LLVM_AR", "LLVM_NM", "LINKER"):
        return tools()[name]
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


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
        code, out = run([tools()["CLANG"], "-c", opt, *CFLAGS, *lang, *extra, *flags, src, "-o", obj])
        return (src, out) if code else None
    with ThreadPoolExecutor(jobs) as pool:
        return [f for f in pool.map(one, jobs_list) if f]
