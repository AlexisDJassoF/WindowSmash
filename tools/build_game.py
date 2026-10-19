#!/usr/bin/env python3
"""Build the game from your own files in one go: what Build WindowSmash.bat runs.

Usage:
  build_game.py [--game <dir>] [--keys <dir>] [--firmware <dir>] [--jobs N]

It does the setup guide's steps (docs/BUILDING.md) one after the other and stops at the first
that fails, saying why. A step whose result is already there is skipped, so running it again
carries on where it stopped.
  1. tools     Python 3.14, and clang-cl (found by tools/cc.py) building a test program
  2. packages  requirements.txt, installed with pip unless they are already
  3. folders   where the game files, keys and firmware are: asked (the answers are the defaults
               next time, in dump/build_folders.txt) unless --game, --keys, --firmware say
  4. program   the Program NCA among the game folder's .nca files (of two, the update's: title
               id ending in 800); its ExeFS into dump/exefs
  5. fonts     the .bfttf fonts of every firmware NCA into dump/fonts (or the six from a fonts
               folder in the game folder)
  6. data      dump/modules/romfs.cfg: the game folder's romfs.bin, checked and set by
               tools/Configure-Game-Data.ps1; without one, the program NCA's own RomFS, which
               must be decrypted (tools/nca.py romfs-cfg)
  7. dlc       dump/modules/aoc.cfg from the game folder's dlc folder, if it has one
  8. fighters  the fighter modules in data.arc into dump/nro (tools/arc.py nro, which skips the
               ones there already)
  9. build     build/ssbu.exe (tools/recomp.py all), unless it is newer than everything it is
               made from; --jobs defaults to what the installed RAM allows: 4 under 16 GB, 8 with
               16 GB, 16 with 32 GB or more
"""
import argparse
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
DUMP = os.path.join(ROOT, "dump")
EXEFS = os.path.join(DUMP, "exefs")
FONTS = os.path.join(DUMP, "fonts")
MODULES = os.path.join(DUMP, "modules")
EXE = os.path.join(ROOT, "build", "ssbu.exe")
SAVED = os.path.join(DUMP, "build_folders.txt")
EXEFS_FILES = ("main", "main.npdm", "rtld", "sdk", "subsdk0")
FONT_FILES = ("nintendo_udsg-r_std_003.bfttf", "nintendo_udsg-r_org_zh-cn_003.bfttf",
              "nintendo_udsg-r_ext_zh-cn_003.bfttf", "nintendo_udjxh-db_zh-tw_003.bfttf",
              "nintendo_udsg-r_ko_003.bfttf", "nintendo_ext_003.bfttf")
PROGRAM = 0   # an NCA's content type (nca.py info's type=Program)


class Failed(Exception):
    """A step that cannot go on; the message says why and what to do."""


def log(msg=""):
    print(msg, flush=True)


def run(args, failure):
    """Run a tool from the port's folder, showing its command; `failure` explains a failed run."""
    log("  " + subprocess.list2cmdline(["python" if a == sys.executable else a for a in args]))
    if subprocess.run(args, cwd=ROOT).returncode:
        raise Failed(failure)


# ---- what the steps decide by (also tested: tests/test_build_game.py) ---------------------
def pick_program(ncas):
    """The program to build from, of [(path, content type, title id)]: the one Program NCA, or
    of two, the update's (its title id ends in 800)."""
    programs = [(path, title) for path, kind, title in ncas if kind == PROGRAM]
    if len(programs) > 1:
        updates = [p for p in programs if p[1] & 0xFFF == 0x800]
        programs = updates if len(updates) == 1 else programs
    if not programs:
        raise Failed("there is no Program NCA among them")
    if len(programs) > 1:
        raise Failed("there are several Program NCAs: " + ", ".join(os.path.basename(p) for p, _ in programs)
                     + ". Keep only the one of the game version to build (setup guide step 2).")
    return programs[0][0]


def jobs_for_ram(gb):
    """recomp.py's --jobs for this much installed RAM, as the setup guide's build step says."""
    return 16 if gb >= 32 else 8 if gb >= 16 else 4


def romfs_cfg_ok(cfg):
    """romfs.cfg is there, and so are the files it reads (plain: the file, then the offset;
    layered: the layer map, then its sources - nca.RomfsImage)."""
    try:
        with open(cfg, encoding="utf-8-sig", errors="replace") as f:
            lines = [l.strip() for l in f if l.strip()]
    except OSError:
        return False
    files = lines[1:] if lines[:1] == ["layered"] else lines[:1]
    return bool(files) and all(os.path.isfile(p) for p in files)


def missing_packages(requirements):
    """The packages of a requirements file that are not installed."""
    from importlib import metadata
    missing = []
    with open(requirements, encoding="utf-8") as f:
        for line in f:
            name = re.split(r"[\s;<>=!~\[#]", line.strip(), maxsplit=1)[0]
            if not name or name.startswith("-"):
                continue
            try:
                metadata.version(name)
            except metadata.PackageNotFoundError:
                missing.append(name)
    return missing


# ---- the steps ------------------------------------------------------------------------------
def check_tools():
    if sys.version_info < (3, 14) or importlib.util.find_spec("compression.zstd") is None:
        raise Failed(f"this is Python {sys.version.split()[0]}: WindowSmash needs Python 3.14 (setup guide step 3).")
    log(f"  Python {sys.version.split()[0]}: {sys.executable}")
    import cc
    clang = cc.CLANG if os.path.isfile(cc.CLANG) else shutil.which(cc.CLANG)
    if not clang:
        raise Failed("Visual Studio's Clang tools (clang-cl.exe) were not found. Install Visual Studio with "
                     "Desktop development with C++ and C++ Clang tools for Windows (setup guide step 4).")
    # the generated C is compiled long after the build starts: try the compiler, the Windows SDK and the linker now
    with tempfile.TemporaryDirectory(prefix="windowsmash-check-") as tmp:
        src = os.path.join(tmp, "check.c")
        with open(src, "w") as f:
            f.write("#include <windows.h>\n#include <stdio.h>\n"
                    "int main(void) { printf(\"%lu\\n\", GetCurrentProcessId()); return 0; }\n")
        lld = ["-fuse-ld=lld"] if os.path.exists(os.path.join(os.path.dirname(clang), "lld-link.exe")) else []
        r = subprocess.run([clang, *lld, *cc.CFLAGS, src, "/Fo" + os.path.join(tmp, "check.obj"),
                            "/Fe" + os.path.join(tmp, "check.exe")], capture_output=True, text=True)
        if r.returncode:
            raise Failed(f"{clang} could not build a test program. Check that Visual Studio has Desktop development "
                         f"with C++ (with a Windows SDK) and C++ Clang tools for Windows (setup guide step 4).\n"
                         + (r.stdout + r.stderr)[-2000:])
    log(f"  clang-cl: {clang}")


def install_packages():
    requirements = os.path.join(ROOT, "requirements.txt")
    missing = missing_packages(requirements)
    if not missing:
        log("  already installed")
        return
    run([sys.executable, "-m", "pip", "install", "-r", "requirements.txt"],
        "pip could not install " + ", ".join(missing) + " (see above).")
    importlib.invalidate_caches()


FOLDERS = {
    "game": ("Folder with your game files", r"D:\MyDump",
             lambda p: None if os.path.isdir(p) else f"There is no folder {p}."),
    "keys": ("Folder with your keys", r"D:\MyKeys",
             lambda p: None if os.path.isfile(os.path.join(p, "prod.keys")) else f"There is no prod.keys in {p}."),
    "firmware": ("Folder with your firmware", r"D:\MyFirmware",
                 lambda p: None if os.path.isdir(p) and any(f.lower().endswith(".nca") for f in os.listdir(p))
                 else f"There are no .nca files in {p}."),
}


def ask_folder(key, given, saved):
    """A folder from the command line, or asked, with last time's answer as the default."""
    label, default, check = FOLDERS[key]
    default = saved.get(key, default)
    while True:
        if given:
            answer = given
        else:
            if not saved.get("hinted"):
                log("  Press Enter to keep the folder in [brackets], or type another.")
                saved["hinted"] = True
            try:
                answer = input(f"  {label} [{default}]: ").strip().strip('"') or default
            except EOFError:   # nobody to answer: the default
                log()
                answer = given = default
        problem = check(answer)
        if not problem:
            saved[key] = os.path.abspath(answer)
            with open(SAVED, "w", encoding="utf-8", newline="\n") as f:
                f.writelines(f"{k}={v}\n" for k, v in saved.items() if k in FOLDERS)
            return saved[key]
        if given:
            raise Failed(problem)
        log(f"  {problem} Try again, or close this window to stop.")


def load_saved():
    saved = {}
    if os.path.exists(SAVED):
        with open(SAVED, encoding="utf-8") as f:
            for line in f:
                key, _, value = line.strip().partition("=")
                if key in FOLDERS and value:
                    saved[key] = value
    return saved


def find_program(game, keys):
    """(path, title id) of the program among the game folder's .nca files (nca.py info's type)."""
    from nca import Nca
    found, unread = [], []
    for name in sorted(os.listdir(game)):
        path = os.path.join(game, name)
        if not name.lower().endswith(".nca") or not os.path.isfile(path):
            continue
        try:
            nca = Nca(path, keys)
        except (SystemExit, Exception) as e:   # (a cut-off file, or one under keys we lack)
            unread.append(f"{name}: {e}")
            continue
        found.append((path, nca.content_type, nca.title_id))
        nca.f.close()
    try:
        program = pick_program(found)
    except Failed as e:
        why = [f"Looked for the game's program among the {len(found) + len(unread)} .nca files in {game}: {e}."]
        if unread:
            why.append("These could not be read (keys missing or too old?):\n    " + "\n    ".join(unread[:8]))
        if not found and not unread:
            why.append("Copy the game's program .nca there (setup guide step 2).")
        raise Failed("\n  ".join(why))
    title = next(t for p, _k, t in found if p == program)
    log(f"  program: {os.path.basename(program)} (title id {title:016X})")
    return program, title


def extract_exefs(program, keys):
    partial = EXEFS + ".partial"
    run([sys.executable, os.path.join("tools", "nca.py"), "exefs", program, "--keys", keys,
         "--out", os.path.join("dump", "exefs.partial")],
        f"Could not copy the program out of {program} (see above).")
    missing = [n for n in EXEFS_FILES if not os.path.isfile(os.path.join(partial, n))]
    if missing:
        raise Failed(f"{os.path.basename(program)} has no {', '.join(missing)}: it is not the game's program.")
    os.makedirs(EXEFS, exist_ok=True)
    for n in os.listdir(partial):
        os.replace(os.path.join(partial, n), os.path.join(EXEFS, n))
    os.rmdir(partial)


def extract_fonts(firmware, keys):
    """What `nca.py romfs-get <nca> --match .bfttf --flat` gets from each firmware NCA, read in
    this one process; an NCA it cannot read (no RomFS, no key) has no fonts for the game."""
    from nca import Nca, copy_out, romfs_entries
    partial = FONTS + ".partial"
    names = sorted(n for n in os.listdir(firmware) if n.lower().endswith(".nca") and os.path.isfile(os.path.join(firmware, n)))
    log(f"  reading the {len(names)} .nca files in {firmware}")
    for name in names:
        nca = None
        try:
            nca = Nca(os.path.join(firmware, name), keys)
            sec = nca.section(0)
            for path, off, size in romfs_entries(sec) if sec else ():
                if ".bfttf" in path:
                    log(f"  {os.path.basename(path)} ({size})")
                    copy_out(sec, off, size, os.path.join(partial, os.path.basename(path)))
        except (SystemExit, Exception):
            continue
        finally:
            if nca:
                nca.f.close()
    if os.path.isdir(partial):
        os.makedirs(FONTS, exist_ok=True)
        for n in os.listdir(partial):
            os.replace(os.path.join(partial, n), os.path.join(FONTS, n))
        os.rmdir(partial)


def missing_fonts(folder):
    return [n for n in FONT_FILES if not os.path.isfile(os.path.join(folder, n))]


def write_romfs_cfg(game, keys, the_program):
    romfs_bin = os.path.join(game, "romfs.bin")
    if os.path.isfile(romfs_bin):
        run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
             os.path.join("tools", "Configure-Game-Data.ps1"), "-NonInteractive", "-RomfsPath", romfs_bin,
             "-FontsDirectory", FONTS],
            f"{romfs_bin} could not be used (see above): it must be the game's data, unencrypted and with "
            "your update applied (setup guide part 2).")
    else:
        program = the_program()[0]
        run([sys.executable, os.path.join("tools", "nca.py"), "romfs-cfg", program, "--keys", keys,
             "--out", os.path.join("dump", "modules", "romfs.cfg")],
            f"There is no romfs.bin in {game}, and the game data inside {os.path.basename(program)} can't be read "
            "in place (see above). Put the game's data there as romfs.bin: unencrypted, with your update "
            "applied (setup guide part 2).")


def build_is_current():
    """build/ssbu.exe is newer than the runtime, the tools and the modules it is built from, and
    has SDL3.dll beside it if SDL3 is there to build with."""
    if not os.path.isfile(EXE):
        return False
    import cc
    if cc.SDL3_DLL and not os.path.isfile(os.path.join(ROOT, "build", "SDL3.dll")):
        return False
    inputs = [os.path.join(ROOT, "runtime", n) for n in os.listdir(os.path.join(ROOT, "runtime"))]
    inputs += [os.path.join(TOOLS, n) for n in os.listdir(TOOLS) if n.endswith(".py")]
    inputs += [os.path.join(EXEFS, n) for n in os.listdir(EXEFS)]
    nro = os.path.join(DUMP, "nro")
    inputs += [os.path.join(nro, n) for n in os.listdir(nro) if n.endswith(".nro")] if os.path.isdir(nro) else []
    return os.path.getmtime(EXE) > max(os.path.getmtime(p) for p in inputs if os.path.isfile(p))


def installed_ram_gb():
    import ctypes
    kb = ctypes.c_ulonglong()
    if not ctypes.windll.kernel32.GetPhysicallyInstalledSystemMemory(ctypes.byref(kb)):
        return None
    return kb.value / (1 << 20)


def in_use(path):
    """A running program's file can't be opened for writing."""
    try:
        with open(path, "r+b"):
            return False
    except FileNotFoundError:
        return False
    except PermissionError:
        return True


def build(a):
    sys.path.insert(0, TOOLS)
    log("[1/9] Check the tools")
    check_tools()

    log("\n[2/9] Install the Python packages")
    install_packages()

    cfg, aoc = os.path.join(MODULES, "romfs.cfg"), os.path.join(MODULES, "aoc.cfg")
    exefs_done = not [n for n in EXEFS_FILES if not os.path.isfile(os.path.join(EXEFS, n))]
    romfs_done = romfs_cfg_ok(cfg)
    log("\n[3/9] Your folders")
    os.makedirs(DUMP, exist_ok=True)
    saved = load_saved()
    game = keys = firmware = fonts_from = None
    if not (exefs_done and romfs_done and os.path.exists(aoc)):
        game = ask_folder("game", a.game, saved)
    if missing_fonts(FONTS):
        fonts_from = "game" if game and not missing_fonts(os.path.join(game, "fonts")) else "firmware"
    if game or fonts_from == "firmware":
        keys = ask_folder("keys", a.keys, saved)
    if fonts_from == "firmware":
        firmware = ask_folder("firmware", a.firmware, saved)
    if not keys:
        log("  none needed: the steps that read them are done")

    program = None

    def the_program():
        nonlocal program
        program = program or find_program(game, keys)
        return program

    log("\n[4/9] Copy the game's program (dump\\exefs)")
    if exefs_done:
        log("  already done")
    else:
        extract_exefs(the_program()[0], keys)

    log("\n[5/9] Take the fonts out of your firmware (dump\\fonts)")
    if fonts_from is None:
        log("  already done")
    elif fonts_from == "game":
        os.makedirs(FONTS, exist_ok=True)
        for n in FONT_FILES:
            shutil.copyfile(os.path.join(game, "fonts", n), os.path.join(FONTS, n))
        log(f"  copied the six fonts from {os.path.join(game, 'fonts')}")
    else:
        extract_fonts(firmware, keys)
        missing = missing_fonts(FONTS)
        if missing:
            raise Failed(f"These fonts were not in {firmware}: {', '.join(missing)}. Check that it holds all of "
                         "your firmware's .nca files, and that your keys are from after your Switch's latest "
                         "system update.")

    log("\n[6/9] Connect your game data (dump\\modules\\romfs.cfg)")
    if romfs_done:
        log("  already done")
    else:
        if os.path.exists(cfg):
            log(f"  {cfg} reads a file that is not there any more: setting it again")
        write_romfs_cfg(game, keys, the_program)

    log("\n[7/9] Turn on your DLC (dump\\modules\\aoc.cfg)")
    dlc = os.path.join(game, "dlc") if game else None
    if os.path.exists(aoc):
        log("  already done")
    elif not os.path.isdir(dlc):
        log(f"  no dlc folder in {game}: no DLC")
    else:
        run([sys.executable, os.path.join("tools", "nca.py"), "aoc-cfg", dlc, "--keys", keys,
             "--out", os.path.join("dump", "modules", "aoc.cfg"), "--app", f"{the_program()[1] & ~0xFFF:016X}"],
            f"Could not read the DLC in {dlc} (see above).")

    log("\n[8/9] Take out the fighters (dump\\nro)")
    run([sys.executable, os.path.join("tools", "arc.py"), "nro"],
        "Could not read the fighters from the game data (see above). Check that the game data from step 6 is "
        "the game's, with your update applied.")

    log("\n[9/9] Build the game (build\\ssbu.exe)")
    import cc
    if build_is_current():
        log("  build\\ssbu.exe is newer than everything it is built from: nothing to do (delete it to build again)")
    else:
        if in_use(EXE):
            raise Failed("The game is running from build\\ssbu.exe: close it, then run this again.")
        jobs = a.jobs
        if not jobs:
            gb = installed_ram_gb()
            jobs = jobs_for_ram(gb) if gb else 4
            log(f"  {jobs} jobs at a time, for {gb:.0f} GB of RAM" if gb else f"  {jobs} jobs at a time (the RAM size is unknown)")
        if not cc.SDL3_DLL:
            log("  (no SDL3 in deps\\SDL3: Xbox controllers and the keyboard only - setup guide's SDL3 step)")
        log("  the first build takes from 30 minutes to a few hours, later ones minutes; if it stops, run this again to carry on")
        run([sys.executable, os.path.join("tools", "recomp.py"), "all", os.path.join("dump", "exefs"),
             "--out", "build", "--jobs", str(jobs)],
            "The build failed (see above). If the PC ran out of memory, build with fewer jobs at a time: open "
            "PowerShell in this folder and run  & \".\\Build WindowSmash.bat\" --jobs 4")
    log("\nDone: build\\ssbu.exe is ready. Start the game with Play SSBU.bat.")


def main():
    ap = argparse.ArgumentParser(description="Build the game from your own files (Build WindowSmash.bat).")
    ap.add_argument("--game", help=r"the folder with the game's files (asked; default D:\MyDump)")
    ap.add_argument("--keys", help=r"the folder with prod.keys and title.keys (asked; default D:\MyKeys)")
    ap.add_argument("--firmware", help=r"the folder with the firmware's .nca files (asked; default D:\MyFirmware)")
    ap.add_argument("--jobs", type=int, help="how many files to compile at a time (default: by the RAM)")
    a = ap.parse_args()
    try:
        build(a)
    except Failed as e:
        log(f"\nERROR: {e}")
        return 1
    except KeyboardInterrupt:
        log("\nStopped. Run this again to carry on from here.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
