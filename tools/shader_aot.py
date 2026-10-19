#!/usr/bin/env python3
"""Compile the game's shaders ahead of time, into the renderer's disk cache.

Usage:
  shader_aot.py [--tree <dir>] [--jobs N] [--dry]

The game's GPU programs are stored in data.arc as they are uploaded: in the effect files
(.eff), the shader databases (.nushdb) and nested archives (.arc). This finds every program
there (by its header word: 256-byte boundaries + 0x30 in effects and archives, 16-byte ones in
databases), keeps one copy of each, pairs each pixel program with the vertex programs of its
file (a vertex program's outputs are its pixel program's inputs), and has
tools/shaderaot.c translate and compile them all into <tree>/dump/shadercache, exactly as the
renderer would: a program's first use in the game then loads its DXBC instead of compiling it.

The files are dated 2001-01-01 until the game first uses them, so the start's prewarm only
makes the shaders the game has used. Programs with indirect branches are left out (their key
depends on a draw's constant buffers), as are compute programs (on their launch's size).
--dry translates without compiling and compares the keys with the cache that is there.
"""
import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import threading
import time
try:
    from compression.zstd import decompress as zstd_decompress   # Python 3.14
except ImportError:                                               # 3.10 to 3.13: the zstandard package
    import zstandard

    def zstd_decompress(data):
        return zstandard.ZstdDecompressor().decompressobj().decompress(data)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import arc as A    # noqa: E402
import cc          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOW40 = (1 << 40) - 1
MAX_PROGRAM = 0x40000   # as the renderer reads


def is_header(w):
    """A vertex (type 1, shader type 1) or pixel (type 2, shader type 5) program's first word:
    header version 3, SASS version 3."""
    t, v, st, sass = w & 0x1F, (w >> 5) & 0x1F, (w >> 10) & 0xF, (w >> 17) & 0xF
    return v == 3 and sass == 3 and ((t == 1 and st == 1) or (t == 2 and st == 5))


def containers(arc):
    """(extension, (offset, stored size, size, flags)) of every distinct .eff/.nushdb/.arc."""
    want = {A.hash40(e): e for e in ("eff", "nushdb", "arc")}
    seen, out = set(), []
    off, size, count = arc.tab["paths"]
    for i in range(count):
        full, ext = struct.unpack_from("<QQ", arc.t, off + i * size)
        e = want.get(ext & LOW40)
        if not e:
            continue
        try:
            info = arc._entry("info_indices", full >> 40, "<2I")[1]
            where = arc._data(arc._entry("infos", info, "<4I")[2])
        except struct.error:
            continue
        if where and where[0] not in seen:
            seen.add(where[0])
            out.append((e, where))
    return out


def find_programs(arc):
    """Distinct programs: list of [stage, bytes, set of paired vertex program ids]."""
    programs, ids = [], {}
    unread = 0
    for ext, (off, stored, size, flags) in containers(arc):
        if not stored or not size:   # (an empty file - data.arc has a few, marked compressed: nothing in it)
            continue
        data = arc.read(off, stored)
        if flags & 3:
            try:
                data = zstd_decompress(data)
            except Exception:
                unread += 1
                continue
        step, start = (16, 0) if ext == "nushdb" else (256, 0x30)
        mv = memoryview(data)
        at = [k for k in range(start, len(data) - 0x60, step) if is_header(int.from_bytes(mv[k:k + 4], "little"))]
        here = ([], [])
        for j, k in enumerate(at):
            end = min(len(data), k + MAX_PROGRAM, at[j + 1] if j + 1 < len(at) else len(data))
            blob = bytes(mv[k:end])
            stage = 0 if blob[0] & 0x1F == 1 else 1
            key = hashlib.blake2b(blob, digest_size=16).digest()
            pid = ids.get(key)
            if pid is None:
                pid = ids[key] = len(programs)
                programs.append([stage, blob, set()])
            here[stage].append(pid)
        # a pixel program is paired with every vertex program of its file: the one before it
        # is right for only about 60 % of the cache's pairs, the file's for about 95 % (the
        # game has only a few dozen distinct vertex output masks, so this stays small)
        for pid in here[1]:
            programs[pid][2].update(here[0])
    return programs, unread


def write_pack(programs, path):
    with open(path, "wb") as f:
        f.write(b"SAOT" + struct.pack("<I", len(programs)))
        for stage, blob, pairs in programs:
            pairs = sorted(pairs)
            f.write(struct.pack(f"<3I{len(pairs)}I", stage, len(blob), len(pairs), *pairs))
            f.write(blob)


def build_tool(exe):
    sources = [os.path.join(ROOT, "tools", "shaderaot.c"), os.path.join(ROOT, "runtime", "gpu_shader.c")]
    headers = [os.path.join(ROOT, "runtime", n) for n in ("gpu_shader.h", "gpu_shader_cache.h")]
    if os.path.exists(exe) and os.path.getmtime(exe) > max(os.path.getmtime(p) for p in sources + headers):
        return
    r = subprocess.run([cc.CLANG, "/nologo", "/O2", *cc.CFLAGS, *sources, "/Fe" + exe, "/link", "d3dcompiler.lib"],
                       capture_output=True, text=True, cwd=os.path.dirname(exe))
    if r.returncode:
        raise SystemExit("cannot build shaderaot:\n" + (r.stdout + r.stderr)[-3000:])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--tree", default=ROOT, help="the game version's folder (default: the port's own)")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument("--dry", action="store_true")
    a = ap.parse_args()
    tree = os.path.abspath(a.tree)
    t0 = time.time()
    arc = A.Arc(os.path.join(tree, "dump", "modules", "romfs.cfg"))
    programs, unread = find_programs(arc)
    nvs = sum(1 for p in programs if p[0] == 0)
    print(f"{len(programs)} distinct programs ({nvs} vertex, {len(programs) - nvs} pixel) in {time.time() - t0:.0f} s"
          + (f"; {unread} files could not be read" if unread else ""), flush=True)
    work = os.path.join(tree, "dump", "aot")
    os.makedirs(work, exist_ok=True)
    pack = os.path.join(work, "programs.pack")
    write_pack(programs, pack)
    exe = os.path.join(ROOT, "build", "shaderaot.exe")
    build_tool(exe)
    cache = os.path.join(tree, "dump", "shadercache")
    os.makedirs(cache, exist_ok=True)
    jobs = 1 if a.dry else a.jobs
    procs = [subprocess.Popen([exe, pack, cache, str(i), str(jobs)] + (["--dry"] if a.dry else []),
                              stdout=subprocess.PIPE if a.dry else subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                              creationflags=getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0))
             for i in range(jobs)]
    if a.dry:
        outs = [p.communicate() for p in procs]
        print("\n".join(line for _out, err in outs for line in err.splitlines() if line.startswith("part ")))
    else:
        print(f"compiling with {jobs} processes at low priority (the PC stays usable; close this window to stop: "
              "what is done stays done)", flush=True)
        parts = follow(procs, Progress(sys.stdout))
        print(summary(parts) or "\n".join(parts))
    if a.dry:
        keys = {tuple(l.split()) for l in outs[0][0].splitlines()}
        have = set()
        for name in os.listdir(cache):
            parts = name[:-5].split("_") if name.endswith(".dxbc") else []
            if len(parts) == 5 and parts[0] == "v" + str(cache_version()) and parts[1] in ("vs", "ps"):
                have.add(("vs", parts[2]) if parts[1] == "vs" else ("ps", parts[2], parts[3]))
        print(f"the cache has {len(have)} vertex/pixel keys; {len(have & keys)} of them are among the {len(keys)} found here")
    print(f"done in {time.time() - t0:.0f} s")


# ---- progress: shaderaot's "progress <done> <of>" lines, from every part, as one bar ------------
def parse_progress(line):
    """(done, of) from a "progress <done> <of>" line, else None."""
    p = line.split()
    if len(p) == 3 and p[0] == "progress" and p[1].isdigit() and p[2].isdigit():
        return int(p[1]), int(p[2])
    return None


def time_left(seconds):
    """A time left, as a person says it."""
    minutes = round(seconds / 60)
    if minutes < 1:
        return "under a minute left"
    if minutes < 60:
        return f"about {minutes} min left"
    return f"about {minutes // 60} h {minutes % 60:02d} min left"


def progress_text(done, of, rate=None, width=30):
    """The bar, the share done, and the time left at `rate` shaders a second (once there is one)."""
    frac = min(1.0, done / of) if of else 1.0
    filled = int(frac * width)
    text = f"[{'#' * filled}{'-' * (width - filled)}] {frac:4.0%}  {done:,} of {of:,} shaders"
    if rate and 0 < done < of:
        text += ", " + time_left((of - done) / rate)
    return text


class Progress:
    """Shows the parts' combined progress: one line drawn over itself in a console, else a line
    every 5 % (for a log)."""

    WINDOW = 120   # seconds of recent pace the time left goes by (the pace picks up after the start)

    def __init__(self, out, console=None):
        self.out = out
        self.console = out.isatty() if console is None else console
        self.samples = []   # (time, shaders done), once every part has said how much it has
        self.ready_said = False
        self.shown = -1     # (the last 5 % step a log line was written for)
        self.last = None
        self.width = 0

    def rate(self, now, done):
        """Shaders a second over the last WINDOW seconds, once there are 10 seconds of them."""
        self.samples.append((now, done))
        while len(self.samples) > 2 and self.samples[1][0] <= now - self.WINDOW:
            self.samples.pop(0)
        t0, d0 = self.samples[0]
        return (done - d0) / (now - t0) if now - t0 >= 10 and done > d0 else None

    def __call__(self, state, now=None, final=False):
        now = time.time() if now is None else now
        if any(of is None for _done, of in state):
            if not self.ready_said:
                self.out.write("getting ready: translating the vertex programs...\n")
                self.out.flush()
                self.ready_said = True
            return
        done, of = sum(d for d, _o in state), sum(o for _d, o in state)
        text = progress_text(done, of, self.rate(now, done))
        if self.console:
            self.out.write("\r" + text.ljust(self.width) + ("\n" if final else ""))
            self.width = len(text)
            self.out.flush()
        else:
            step = int(100 * done / of) // 5 if of else 20
            if step > self.shown or (final and text != self.last):
                self.out.write(text + "\n")
                self.out.flush()
                self.shown, self.last = step, text


def follow(procs, show, every=1.0):
    """Waits for the parts, showing their progress as it comes; returns their last lines ("part ...")."""
    state = [(0, None)] * len(procs)
    lasts = []

    def read(i, p):
        for line in p.stderr:
            got = parse_progress(line)
            if got:
                state[i] = got
            elif line.startswith("part "):
                lasts.append(line.rstrip())
    readers = [threading.Thread(target=read, args=(i, p), daemon=True) for i, p in enumerate(procs)]
    for r in readers:
        r.start()
    while any(r.is_alive() for r in readers):
        show(state)
        time.sleep(every)
    for p in procs:
        p.wait()
    show([(d, o if o is not None else d) for d, o in state], final=True)
    return lasts


def summary(lines):
    """The parts' last lines added up into one, or None if they don't read as expected."""
    fields = ("keys", "compiled", "already cached", "with jump tables", "fxc failures", "not translated")
    total = dict.fromkeys(fields, 0)
    for line in lines:
        m = re.search(r": (\d+) keys; (\d+) compiled, (\d+) already cached, (\d+) with jump tables \(left out\), "
                      r"(\d+) fxc failures, (\d+) not translated", line)
        if not m:
            return None
        for k, v in zip(fields, m.groups()):
            total[k] += int(v)
    return (f"{total['keys']:,} shaders: {total['compiled']:,} compiled, {total['already cached']:,} already there, "
            f"{total['with jump tables']:,} left out (jump tables), {total['fxc failures']:,} that fxc could not compile, "
            f"{total['not translated']:,} not translated")


def cache_version():
    text = open(os.path.join(ROOT, "runtime", "gpu_shader_cache.h"), encoding="utf-8").read()
    return int(re.search(r"#define SHADER_CACHE_VERSION (\d+)", text).group(1))


if __name__ == "__main__":
    main()
