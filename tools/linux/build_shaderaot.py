#!/usr/bin/env python3
"""Build tools/shaderaot.c for Linux: the ahead-of-time shader compiler, compiling with
vkd3d-shader as the Linux build's renderer does (runtime/linux/d3dcompile_vkd3d.c), so that its
cache files are the ones the game would write itself.

Usage: build_shaderaot.py [--out <file>]   (default: build-linux-tools/shaderaot)

Needs the toolchain of build_linux.py and vkd3d-shader built (build_vkd3d.py linux). Then:
  shaderaot <pack> <cache dir> <part> <parts> [--dry] [--keys <file>] [--progress] [--die-with-parent]"""
import os
import sys

from toolchain import CFLAGS, DXVK_INCLUDE, LINKER, REPO, RUNTIME, SYSROOT, VKD3D_BUILD, VKD3D_SRC, compile_each, run, tools

INCLUDES = ["-I", RUNTIME, "-I", os.path.join(RUNTIME, "linux", "include"), "-I", os.path.join(DXVK_INCLUDE, "windows"),
            "-I", os.path.join(DXVK_INCLUDE, "directx"), "-I", os.path.join(VKD3D_SRC, "include")]
SOURCES = [os.path.join(REPO, "tools", "shaderaot.c"), os.path.join(RUNTIME, "gpu_shader.c"), os.path.join(RUNTIME, "gpu_shader_cfg.c"),
           os.path.join(RUNTIME, "linux", "d3dcompile_vkd3d.c")]


def build(out):
    vkd3d = os.path.join(VKD3D_BUILD, "linux", "libvkd3d-shader.a")
    if not os.path.exists(vkd3d):
        raise SystemExit("vkd3d-shader is not built: run tools/linux/build_vkd3d.py linux")
    objdir = os.path.join(os.path.dirname(out), "obj-shaderaot")   # (not shadertest's: both may build at once)
    jobs = [(src, os.path.join(objdir, os.path.basename(src)[:-2] + ".o"), "-O2", INCLUDES + ["-D_GNU_SOURCE"]) for src in SOURCES]
    failures = compile_each(jobs)
    for src, err in failures:
        print(f"FAILED {src}:\n{err[:3000]}")
    if failures:
        return False
    code, text = run([tools()["CLANG"], "--target=x86_64-linux-gnu", "--sysroot=" + SYSROOT, *LINKER, "-no-pie",
                      *[obj for _src, obj, _opt, _flags in jobs], vkd3d, "-lm", "-lpthread", "-ldl", "-o", out])
    if code:
        print("link failed\n" + text[:4000])
        return False
    print(f"built {out}")
    return True


def main():
    out = os.path.abspath(sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else
                          os.path.join(REPO, "build-linux-tools", "shaderaot"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    return 0 if build(out) else 1


if __name__ == "__main__":
    sys.exit(main())
