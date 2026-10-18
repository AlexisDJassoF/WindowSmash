#!/usr/bin/env python3
"""Static recompiler driver.

Usage:
  recomp.py coverage <nso> [--jobs N]
  recomp.py emit     <nso> --roots <off>[,<off>...] --out <file.c> [--depth N]
  recomp.py emit     <nso> --sample N --out <file.c>
  recomp.py image    <nso> --out <file.bin>
  recomp.py build    <nso> --out <dir> [--jobs N] [--per-file INSNS]
  recomp.py compile  <dir> [--jobs N]
  recomp.py link     --dirs <dir>[,<dir>...] --out <exe>
  recomp.py all      <exefs dir> --out <build dir> [--jobs N]

Every command also takes --tree <dir>: the folder whose dump/ (module images, harvested NROs)
and build/ (scratch images) it works with - the port's own (default), or a game version's
folder of the same layout, e.g. v13.0.5 (tools/update.py).

coverage  lift every function and report what the lifter cannot translate yet
emit      write C for the given functions and everything they call directly
image     write the module image with base relocations applied for its load address
build     write C for a whole module, split across many files, plus its loader tables
compile   compile a build directory to objects (incremental; reports per-file failures)
link      compile the host runtime and link module build directories into an executable
all       image + build + compile every module of an ExeFS, then link

Each module is recompiled for a fixed load address (see MODULES); a function is named after
its guest address, so names are unique across modules. A module function whose exported
name has a native implementation - `HLE(name)` in runtime/*.c - is emitted as a forwarder to
it, so the replacement is reached by every caller, including direct calls inside the module.
"""
import bisect
import mmap
import os
import re
import struct
import sys
import time
from collections import Counter
from multiprocessing import Pool

from funcs import discover
from lift import Lifter
from nso import Module

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TREE = ROOT   # --tree
# Load addresses, in the order the console's loader places (and searches) the modules.
MODULES = {"main": 0x7100000000, "subsdk0": 0x7108000000, "sdk": 0x7109000000}
# Modules the game loads at run time (NROs). The runtime saves each one it is handed that is
# not compiled in yet as dump/nro/<build id>.nro; each is then given a load address of its
# own for good, recorded in dump/nro/bases.txt, so that rebuilding never moves a module.
NRO_DIR = os.path.join(TREE, "dump", "nro")
NRO_BASE, NRO_ALIGN = 0x7200000000, 0x1000000
_state = {}


def module_key(path):
    return os.path.splitext(os.path.basename(path))[0]


def nro_modules():
    """name -> base for every harvested NRO, naming and placing new arrivals."""
    manifest = os.path.join(NRO_DIR, "bases.txt")
    rows = [l.split() for l in open(manifest)] if os.path.exists(manifest) else []
    known = {r[3]: r for r in rows}
    end = max([int(r[2], 16) for r in rows] + [NRO_BASE])
    added = False
    for f in sorted(os.listdir(NRO_DIR)) if os.path.isdir(NRO_DIR) else []:
        path = os.path.join(NRO_DIR, f)
        if not f.endswith(".nro") or module_key(f) in {r[0] for r in rows}:
            continue
        mod = Module(path)
        if mod.build_id in known:   # saved again under its id before it was compiled in
            os.remove(path)
            continue
        name = re.sub(r"\W", "_", module_key(mod.module_name().replace("\\", "/")).lower()) or "nro"
        if not name[0].isalpha():
            name = "nro_" + name
        while name in MODULES or name in {r[0] for r in rows}:
            name += "_"
        base = (end + NRO_ALIGN - 1) // NRO_ALIGN * NRO_ALIGN
        end = base + mod.mem_end
        row = [name, f"{base:x}", f"{end:x}", mod.build_id]
        rows.append(row)
        known[mod.build_id] = row
        os.replace(path, os.path.join(NRO_DIR, name + ".nro"))
        print(f"new NRO {name}: {mod.mem_end:,} bytes at {base:#x}")
        added = True
    if added:
        with open(manifest, "w", newline="\n") as f:
            f.writelines(" ".join(r) + "\n" for r in rows)
    return {r[0]: int(r[1], 16) for r in rows}


def base_of(path):
    if arg("--base"):
        return int(arg("--base"), 16)
    key = module_key(path)
    if key not in MODULES and path.endswith(".nro"):
        MODULES.update(nro_modules())
    return MODULES[key]


def hle_names():
    """Exported names that have a native implementation in the runtime sources."""
    names = set()
    runtime = os.path.join(ROOT, "runtime")
    for name in os.listdir(runtime):
        if name.endswith(".c"):
            with open(os.path.join(runtime, name), encoding="utf-8") as f:
                names |= set(re.findall(r"^HLE\((\w+)\)", f.read(), re.M))
    return names


def hle_wraps():
    """Guest functions (not exported) wrapped natively: `HLE_WRAP(<address>)` in the runtime
    sources. The function stays, as f_<address>_orig; its callers reach the native wrapper."""
    found = set()
    runtime = os.path.join(ROOT, "runtime")
    for name in os.listdir(runtime):
        if name.endswith(".c"):
            with open(os.path.join(runtime, name), encoding="utf-8") as f:
                found |= {int(a, 16) for a in re.findall(r"^HLE_WRAP\(([0-9a-fA-F]+)\)", f.read(), re.M)}
    return found


def write_if_changed(path, text):
    """Leave an identical file untouched so its object stays up to date."""
    if os.path.exists(path):
        with open(path, encoding="utf-8", newline="") as f:
            if f.read() == text:
                return False
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return True


def _init(image_path, entries, base, forward, setjmp=None, pads=None):
    f = open(image_path, "rb")
    _state["img"] = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    _state["args"] = (entries, base, forward, setjmp, pads)


def unwind_jumpto(mod, found):
    """libunwind's Registers_arm64::jumpto: loads every register from the context it is given
    and returns into the frame being resumed. That cannot work as a host return, so it is
    replaced natively; found by its code (ldp x2, x3, [x0, #0x10] ... ldr x30, [x0, #0x100])."""
    text = bytes(mod.image[mod.text.mem:mod.text.end])
    for m in re.finditer(re.escape(struct.pack("<I", 0xA9410C02)), text):
        if m.start() % 4:
            continue
        a = mod.text.mem + m.start()
        if a in found and struct.pack("<I", 0xF940801E) in text[m.start():m.start() + 0x40]:
            return {a: "rt_unwind_jumpto"}
    return {}


SETJMP_NAMES = {"setjmp", "_setjmp", "__setjmp"}


def setjmp_targets(mod):
    """Where calls to setjmp land in this module: its own definitions and its import stubs."""
    from funcs import import_stubs
    targets = {v for n, v, _s, info, _x in mod.exports if n in SETJMP_NAMES}
    slots = {r[0]: mod.symbols[r[2]][0] for r in mod.relocs if r[2]}
    targets |= {stub for stub, slot in import_stubs(mod).items() if slots.get(slot) in SETJMP_NAMES}
    return targets


def _lifter():
    return Lifter(_state["img"], *_state["args"])


def _cover(funcs):
    lifter = _lifter()
    ok = ok_insns = insns = bad_insns = 0
    reasons = Counter()
    blocked = Counter()
    for start, size in funcs:
        _code, why, _callees = lifter.lift_function(start, size)
        insns += size // 4
        if why:
            bad_insns += sum(why.values())
            reasons.update(why)
            kinds = {r for r, _w in why}
            if len(kinds) == 1:
                blocked[next(iter(kinds))] += 1
        else:
            ok += 1
            ok_insns += size // 4
    return (ok, ok_insns, insns, bad_insns, reasons, blocked, lifter.tables, lifter.indirect_tail,
            lifter.table_like, lifter.unresolved, lifter.assumed)


def _emit(args):
    index, funcs, outdir = args
    lifter = _lifter()
    own = {a for a, _ in funcs}
    bodies, ext, bad = [], set(), 0
    for start, size in funcs:
        code, why, callees = lifter.lift_function(start, size)
        bad += sum(why.values())
        bodies.append(code)
        ext |= callees
    name = f"g{index:04d}.c"
    text = ('#include "recomp.h"\n\n' + "".join(f"GuestFn f_{lifter.base + a:x};\n" for a in sorted(ext | own))
            + "\n" + "\n\n".join(bodies) + "\n")
    write_if_changed(os.path.join(outdir, name), text)
    insns = sum(s for _, s in funcs) // 4
    return name, ext - own, insns, max(s for _, s in funcs) // 4, bad, lifter.unresolved


def load(path):
    """(module, {entry: sources}, [(entry, size)]) with every function that has code."""
    mod = Module(path)
    found, sizes = discover(mod)
    funcs = sorted((a, sizes[a]) for a, src in found.items() if "eh" in src or "plt" in src)
    # Hand-written assembly (system-call wrappers, memcpy, setjmp, ...) has no unwind entry.
    # Such a routine runs from its entry to the next known entry or described function,
    # less any trailing padding.
    starts = [a for a, _ in funcs]
    ends = [a + s for a, s in funcs]

    def described(a):
        k = bisect.bisect_right(starts, a) - 1
        return k >= 0 and a < ends[k]

    # Compiled code also reaches such routines by tail-jump alone; a `b` out of a described
    # function to somewhere no description covers is an entry too. (A `b` inside an assembly
    # routine is left alone: those are its own internal labels.)
    text = bytes(mod.image[mod.text.mem:mod.text.end])
    for m in re.finditer(rb"[\x14-\x17]", text[3::4]):
        src = mod.text.mem + m.start() * 4
        imm = struct.unpack_from("<I", text, m.start() * 4)[0] & 0x3FFFFFF
        target = src + (imm - 0x4000000 if imm & 0x2000000 else imm) * 4
        if mod.text.mem <= target < mod.text.end and target not in found and described(src) and not described(target):
            found[target] = {"b"}
    marks = sorted(set(found) | set(starts) | {mod.text.end})
    extra = []
    for a in sorted(found):
        if "eh" in found[a] or "plt" in found[a] or not mod.text.mem <= a < mod.text.end or a & 3:
            continue
        if described(a):
            continue   # an address inside a described function: a data pointer, not an entry
        end = marks[bisect.bisect_right(marks, a)]
        while end > a and mod.u32(end - 4) in (0, 0xD503201F, 0xE7FFDEFE):
            end -= 4
        if end > a:
            extra.append((a, end - a))
    return mod, found, sorted(funcs + extra)


def image_file(mod):
    build_dir = os.path.join(TREE, "build")
    os.makedirs(build_dir, exist_ok=True)
    path = os.path.join(build_dir, f"image-{mod.build_id[:16]}.img")
    if not os.path.exists(path) or os.path.getsize(path) != len(mod.image):
        with open(path, "wb") as f:
            f.write(mod.image)
    return path


def coverage(path, jobs):
    import capstone
    mod, found, funcs = load(path)
    step = 400
    chunks = [funcs[i:i + step] for i in range(0, len(funcs), step)]
    ok = ok_insns = insns = bad = tables = tails = table_like = assumed = 0
    reasons, blocked, unresolved = Counter(), Counter(), []
    with Pool(jobs, _init, (image_file(mod), set(found), base_of(path), {})) as pool:
        for r in pool.imap_unordered(_cover, chunks):
            ok += r[0]; ok_insns += r[1]; insns += r[2]; bad += r[3]
            reasons.update(r[4]); blocked.update(r[5])
            tables += r[6]; tails += r[7]; table_like += r[8]; unresolved += r[9]; assumed += r[10]

    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    by_kind, by_mnem = Counter(), Counter()
    for (why, w), n in reasons.items():
        by_kind[why] += n
        d = next(cs.disasm_lite(struct.pack("<I", w), 0), None)
        by_mnem[(why, d[2] if d else "?")] += n

    print(f"functions lifted completely : {ok:,} / {len(funcs):,} ({ok * 100 / len(funcs):.1f}%)")
    print(f"instructions lifted         : {insns - bad:,} / {insns:,} ({(insns - bad) * 100 / insns:.2f}%)")
    print(f"instructions in clean funcs : {ok_insns:,} ({ok_insns * 100 / insns:.1f}%)")
    print(f"jump tables resolved        : {tables:,} of {table_like:,} table-style dispatches"
          f" ({assumed:,} by validation rather than proof)    br treated as indirect tail call: {tails:,}")
    if unresolved:
        print("unresolved table dispatches   : " + " ".join(f"{a:#x}" for a in sorted(unresolved)[:40]))
    if by_kind:
        print("\nunsupported instructions by reason:")
        for why, n in by_kind.most_common():
            print(f"  {n:>9,}  {why:<42} sole blocker of {blocked[why]:,} functions")
        print("\ntop unsupported mnemonics:")
        for i, ((why, m), n) in enumerate(by_mnem.most_common(48)):
            print(f"  {m:<9}{n:>8,}", end="\n" if i % 6 == 5 else "")
        print()


def emit(path, roots, out, depth, sample=0):
    mod, found, funcs = load(path)
    sizes = dict(funcs)
    lifter = Lifter(mod.image, set(found), base_of(path))
    base = lifter.base
    if sample:   # every k-th function that lifts cleanly, callees left as stubs
        clean = [a for a, size in funcs[::max(1, len(funcs) // (sample * 2))]
                 if not lifter.lift_function(a, size)[1]]
        roots, depth = clean[:sample], 0
    bodies, stubs, seen = {}, set(), set()
    frontier = list(roots)
    for _ in range(depth + 1):
        nxt = []
        for a in frontier:
            if a in seen:
                continue
            seen.add(a)
            if a not in sizes:
                stubs.add(a)
                continue
            code, why, callees = lifter.lift_function(a, sizes[a])
            if why:
                print(f"  f_{base + a:x}: {sum(why.values())} unsupported instruction(s) -> traps at runtime")
            bodies[a] = code
            nxt.extend(callees)
        frontier = nxt
    stubs |= {a for a in frontier if a not in seen}
    with open(out, "w") as f:
        f.write('#include "recomp.h"\n\n')
        for a in sorted(set(bodies) | stubs):
            f.write(f"void f_{base + a:x}(Cpu *c);\n")
        f.write("\n")
        for a in sorted(stubs):
            f.write(f'void f_{base + a:x}(Cpu *c) {{ rt_trap(c, 0x{base + a:X}ull, "function not recompiled"); }}\n')
        for a in sorted(bodies):
            f.write("\n" + bodies[a] + "\n")
        f.write("\nconst struct RtFunc { u64 addr; GuestFn *fn; } rt_funcs[] = {\n")
        for a in sorted(bodies):
            f.write(f"    {{0x{base + a:X}ull, f_{base + a:x}}},\n")
        f.write(f"}};\nconst u32 rt_func_count = {len(bodies)};\n")
    print(f"wrote {out}: {len(bodies)} functions, {len(stubs)} stubs")


def image(path, out):
    mod, base = Module(path), base_of(path)
    img = bytearray(mod.image) + bytes(mod.mem_end - len(mod.image))
    n = 0
    for off, typ, _sym, addend in mod.relocs:
        if typ == 1027:
            struct.pack_into("<Q", img, off, base + addend)
            n += 1
    with open(out, "wb") as f:
        f.write(img)
    print(f"wrote {out}: {len(img):,} bytes, {n:,} base relocations applied for {base:#x}")


def module_table(mod, name, base, funcs, missing):
    """C source for a module's loader tables: functions, exports, and symbol relocations."""
    imports, relocs = {}, []
    for off, typ, sym, addend in mod.relocs:
        if typ == 1027:
            continue
        sname, _value, _size, info, _shndx = mod.symbols[sym]
        index = imports.setdefault(sname, (len(imports), info >> 4 == 2))[0]
        relocs.append((off, addend, index))
    defined = [a for a, _ in funcs]
    out = ['#include "module.h"\n\n']
    out += [f"GuestFn f_{base + a:x};\n" for a in defined]
    out += [f'void f_{base + a:x}(Cpu *c) {{ rt_trap(c, 0x{base + a:X}ull, "no code at call target"); }}\n'
            for a in sorted(missing)]
    out.append("\nstatic const RtFunc funcs[] = {\n")
    out += [f"    {{0x{base + a:X}ull, f_{base + a:x}}},\n" for a in defined]
    out.append("};\nstatic const RtSym exports[] = {\n")
    out += [f'    {{"{n}", 0x{base + v:X}ull}},\n' for n, v, _s, _i, _x in mod.exports]
    out.append("};\nstatic const char *const imports[] = {\n")
    out += [f'    "{n}",\n' for n in imports]
    out.append("    0\n};\nstatic const u8 import_weak[] = {" + ",".join(str(int(w)) for _i, w in imports.values()) + ",0};\n")
    out.append("static const RtReloc relocs[] = {\n")
    out += [f"    {{0x{o:X}ull, {a}, {i}}},\n" for o, a, i in relocs]
    out.append("    {0, 0, 0}\n};\n\n")
    out.append(f"const RtModule rt_module_{name} = {{\n"
               f'    "{name}", 0x{base:X}ull, 0x{mod.mem_end:X}ull, 0x{mod.text.end:X}ull, 0x{base + mod.dyn.get(12, 0):X}ull,\n'
               f"    funcs, {len(defined)}, exports, {len(mod.exports)}, imports, import_weak, {len(imports)}, "
               f'relocs, {len(relocs)}, "{mod.build_id}",\n}};\n')
    return "".join(out), len(imports), len(relocs)


def build(path, outdir, jobs, per_file):
    mod, found, funcs = load(path)
    name, base = module_key(path), base_of(path)
    os.makedirs(outdir, exist_ok=True)
    hle = hle_names()
    forward = {v: f"hle_{n}" for n, v, _s, info, _x in mod.exports if n in hle and info & 0xF == 2}
    forward.update(unwind_jumpto(mod, found))
    starts = set(found)
    forward.update({a - base: ("wrap", f"hle_wrap_{a:x}") for a in hle_wraps() if a - base in starts})
    from eh import landing_pads
    pads = landing_pads(mod)
    chunks, cur, count = [], [], 0
    for a, size in funcs:   # a function bigger than the budget gets a file to itself
        if cur and count + size // 4 > per_file:
            chunks.append(cur)
            cur, count = [], 0
        cur.append((a, size))
        count += size // 4
    if cur:
        chunks.append(cur)
    defined = {a for a, _ in funcs}
    missing, manifest, bad, unresolved = set(), [], 0, []
    with Pool(jobs, _init, (image_file(mod), set(found), base, forward, setjmp_targets(mod), pads)) as pool:
        tasks = [(i, c, outdir) for i, c in enumerate(chunks)]
        for fname, ext, insns, biggest, b, unres in pool.imap_unordered(_emit, tasks):
            missing |= ext - defined
            manifest.append((fname, insns, biggest))
            bad += b
            unresolved += unres
    table, imports, relocs = module_table(mod, name, base, funcs, missing)
    write_if_changed(os.path.join(outdir, "g_module.c"), table)
    keep = {n for n, _i, _b in manifest} | {"g_module.c"}
    for old in os.listdir(outdir):   # drop files left over from a different partitioning
        if old.startswith("g") and old.endswith((".c", ".obj")) and os.path.splitext(old)[0] + ".c" not in keep:
            os.remove(os.path.join(outdir, old))
    with open(os.path.join(outdir, "manifest.tsv"), "w", newline="\n") as f:
        f.write("".join(f"{n}\t{i}\t{b}\n" for n, i, b in sorted(manifest)))
    size = sum(os.path.getsize(os.path.join(outdir, n)) for n, _i, _b in manifest)
    print(f"{name}: {len(manifest)} files, {size / 1048576:,.0f} MB of C, {len(defined):,} functions, "
          f"{sum(i for _n, i, _b in manifest):,} instructions; {bad} untranslated, {len(missing)} calls to nowhere, "
          f"{len(unresolved)} unresolved jump tables, {len(forward)} replaced natively; "
          f"{imports:,} symbols referenced by {relocs:,} relocations")


def compile_dir(outdir, jobs):
    import cc
    with open(os.path.join(outdir, "manifest.tsv")) as f:
        rows = [line.split("\t") for line in f.read().splitlines()]
    todo = []
    for name, _insns, biggest in rows + [["g_module.c", "0", "0"]]:
        src = os.path.join(outdir, name)
        obj = src[:-2] + ".obj"
        if not os.path.exists(obj) or os.path.getmtime(obj) < os.path.getmtime(src):
            # very large functions make the optimiser superlinear; build those at a lower level
            todo.append((src, obj, "/O1" if int(biggest) > 20000 or name == "g_module.c" else "/O2"))
    t0 = time.time()
    failures = cc.compile_each(todo, jobs)
    objs = [n for n in os.listdir(outdir) if n.endswith(".obj")]
    total = sum(os.path.getsize(os.path.join(outdir, n)) for n in objs)
    print(f"{os.path.basename(outdir)}: compiled {len(todo) - len(failures)} of {len(todo)} stale files in "
          f"{time.time() - t0:,.0f}s; {len(objs)} objects, {total / 1048576:,.0f} MB")
    for src, err in failures[:5]:
        print(f"FAILED {os.path.basename(src)}:\n{err[:1500]}")
    return not failures


def link(dirs, exe):
    """Compile the host runtime and link it with the given module build directories."""
    import cc
    t0 = time.time()
    rt_dir = os.path.join(os.path.dirname(exe), "rt")
    os.makedirs(rt_dir, exist_ok=True)
    headers = max(os.path.getmtime(os.path.join(cc.RUNTIME, n)) for n in os.listdir(cc.RUNTIME) if n.endswith(".h"))
    sources = [n for n in os.listdir(cc.RUNTIME) if re.fullmatch(r"(host|kernel|gpu\w*|hle\w*|ipc\w*|debug\w*|net\w*)\.c", n)]
    jobs = []
    for n in sources:
        src, obj = os.path.join(cc.RUNTIME, n), os.path.join(rt_dir, n[:-2] + ".obj")
        if not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(src), headers):
            jobs.append((src, obj, "/O2"))
    # Native implementations by name, for symbols no module defines.
    names = sorted(hle_names())
    table = os.path.join(rt_dir, "hle_table.c")
    changed = write_if_changed(table, '#include "host.h"\n\n' + "".join(f"GuestFn hle_{n};\n" for n in names)
                               + "const HleEntry hle_table[] = {\n" + "".join(f'    {{"{n}", hle_{n}}},\n' for n in names)
                               + f"    {{0, 0}}\n}};\nconst u32 hle_count = {len(names)};\n")
    if changed or not os.path.exists(table[:-2] + ".obj"):
        jobs.append((table, table[:-2] + ".obj", "/O1"))
    # The run-time loadable modules (NROs) linked in, for nn::ro to find by build id.
    nros = [os.path.basename(os.path.normpath(d)) for d in dirs
            if os.path.basename(os.path.dirname(os.path.normpath(d))) == "nro"]
    nro_table = os.path.join(rt_dir, "nro_table.c")
    changed = write_if_changed(nro_table, '#include "module.h"\n\n'
                               + "".join(f"extern const RtModule rt_module_{n};\n" for n in nros)
                               + "const RtModule *const rt_nro_modules[] = {"
                               + "".join(f"&rt_module_{n}, " for n in nros) + "0};\n"
                               + f"const u32 rt_nro_count = {len(nros)};\n")
    if changed or not os.path.exists(nro_table[:-2] + ".obj"):
        jobs.append((nro_table, nro_table[:-2] + ".obj", "/O1"))
    failures = cc.compile_each(jobs, 4)
    for src, err in failures:
        print(f"FAILED {os.path.basename(src)}:\n{err[:4000]}")
    if failures:
        return False
    objs = [os.path.join(rt_dir, n[:-2] + ".obj") for n in sources] + [table[:-2] + ".obj", nro_table[:-2] + ".obj"]
    for d in dirs:
        objs += sorted(os.path.join(d, n) for n in os.listdir(d) if n.endswith(".obj"))
    cc.link(objs, exe)
    print(f"linked {len(objs)} objects into {exe} ({os.path.getsize(exe) / 1048576:,.0f} MB) in {time.time() - t0:,.0f}s")
    return True


def build_all(exefs, out, jobs):
    dirs = []
    nros = nro_modules()
    MODULES.update(nros)
    for name in MODULES:
        nso = os.path.join(NRO_DIR, name + ".nro") if name in nros else os.path.join(exefs, name)
        target = os.path.join(out, "nro", name) if name in nros else os.path.join(out, name)
        img = os.path.join(TREE, "dump", "modules", name + ".reloc.bin")
        if not os.path.exists(img) or os.path.getmtime(img) < os.path.getmtime(nso):
            image(nso, img)
        build(nso, target, jobs, 40000)
        if not compile_dir(target, min(jobs, 16)):
            return False
        dirs.append(target)
    return link(dirs, os.path.join(out, "ssbu.exe"))


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    if arg("--tree"):
        TREE = os.path.abspath(arg("--tree"))
        NRO_DIR = os.path.join(TREE, "dump", "nro")
    cmd, nso = sys.argv[1], sys.argv[2]
    if cmd == "coverage":
        coverage(nso, int(arg("--jobs", "20")))
    elif cmd == "emit":
        roots = [int(r, 16) for r in arg("--roots", "").split(",") if r]
        emit(nso, roots, arg("--out"), int(arg("--depth", "8")), int(arg("--sample", "0")))
    elif cmd == "image":
        image(nso, arg("--out"))
    elif cmd == "build":
        build(nso, arg("--out"), int(arg("--jobs", "20")), int(arg("--per-file", "40000")))
    elif cmd == "compile":
        sys.exit(0 if compile_dir(nso, int(arg("--jobs", "16"))) else 1)
    elif cmd == "link":
        sys.exit(0 if link(arg("--dirs").split(","), arg("--out")) else 1)
    elif cmd == "all":
        sys.exit(0 if build_all(nso, arg("--out"), int(arg("--jobs", "20"))) else 1)
    else:
        sys.exit(__doc__)
