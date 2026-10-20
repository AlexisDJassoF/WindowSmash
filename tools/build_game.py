#!/usr/bin/env python3
"""Build the game from your own files in one go: what launchers\\Build WindowSmash.bat runs.

Usage:
  build_game.py [--game <dir>] [--keys <dir>] [--firmware <dir>] [--jobs N] [--prepare-only]
  build_game.py --check

It does the setup guide's steps (docs/BUILDING.md) one after the other and stops at the first
that fails, saying why. A step whose result is already there is skipped, so running it again
carries on where it stopped. --check does the first step only (the installer uses it);
--prepare-only all but the last.
  1. tools     Python 3.10 or newer, and clang-cl (found by tools/cc.py) building a test program
  2. packages  requirements.txt, installed with pip unless they are already
  3. folders   where the game files, keys and firmware are: asked (the answers are the defaults
               next time, in dump/build_folders.txt) unless --game, --keys, --firmware say
  4. program   the Program NCA among the game folder's files (tools/gamefiles.py: the guide's
               folder, an emulator's installed-content folder, an update .nsp; of base and
               update, the update's); its ExeFS into dump/exefs, and which game and version it
               is into dump/game_source.txt - a later run with a game folder holding another
               version stops rather than mix the two (one folder, one version: docs/VERSIONS.md)
  5. fonts     the .bfttf fonts of every firmware NCA into dump/fonts (or the six from a fonts
               folder in the game folder)
  6. data      dump/modules/romfs.cfg: the game folder's romfs.bin, checked and set by
               tools/Configure-Game-Data.ps1; without one, the program NCA's own RomFS, read in
               place when it is decrypted (tools/nca.py romfs-cfg), else decrypted first into
               dump/modules (tools/nca.py decrypt); an update's, which is a patch, is applied
               over the base's next to it (tools/update.py apply: a layered romfs.cfg)
  7. dlc       dump/modules/aoc.cfg from the game folder's dlc folder (DLC .nsp files, or
               installed content), or from the DLC installed next to the game (an emulator's)
  8. fighters  the fighter modules in data.arc into dump/nro (tools/arc.py nro, which skips the
               ones there already)
  9. build     build/ssbu.exe (tools/recomp.py all), unless it is newer than everything it is
               made from; --jobs defaults to what the installed RAM allows: 4 under 16 GB, 8 with
               16 GB, 16 with 32 GB or more

On Linux (what install-windowsmash.sh runs), step 1 checks clang, lld, bison and flex, the parts
tools/linux/toolchain.py names, and a test link with SDL2, libopus and libvpx; step 9 builds
build-linux/ssbu: the C generated into build/ (tools/recomp.py all --generate-only), compiled and
linked for Linux (tools/linux/build_vkd3d.py once, then tools/linux/build_linux.py).
"""
import argparse
import errno
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
ROOT = os.path.dirname(TOOLS)
LINUX = sys.platform.startswith("linux")   # (the game built for Linux, on Linux: tools/linux)
DUMP = os.path.join(ROOT, "dump")
EXEFS = os.path.join(DUMP, "exefs")
FONTS = os.path.join(DUMP, "fonts")
MODULES = os.path.join(DUMP, "modules")
EXE = os.path.join(ROOT, "build-linux", "ssbu") if LINUX else os.path.join(ROOT, "build", "ssbu.exe")
SAVED = os.path.join(DUMP, "build_folders.txt")
SOURCE = os.path.join(DUMP, "game_source.txt")   # which game and version dump/ was set up from
EXEFS_FILES = ("main", "main.npdm", "rtld", "sdk", "subsdk0")
FONT_FILES = ("nintendo_udsg-r_std_003.bfttf", "nintendo_udsg-r_org_zh-cn_003.bfttf",
              "nintendo_udsg-r_ext_zh-cn_003.bfttf", "nintendo_udjxh-db_zh-tw_003.bfttf",
              "nintendo_udsg-r_ko_003.bfttf", "nintendo_ext_003.bfttf")
PROGRAM = 0   # an NCA's content type (nca.py info's type=Program)


class Failed(Exception):
    """A step that cannot go on; the message says why and what to do."""


def log(msg=""):
    print(msg, flush=True)


def shown(path):
    """A path as this system writes it (the messages name them the Windows way)."""
    return path.replace("\\", "/") if LINUX else path


def run(args, failure):
    """Run a tool from the port's folder, showing its command; `failure` explains a failed run."""
    log("  " + subprocess.list2cmdline(["python" if a == sys.executable else a for a in args]))
    if subprocess.run(args, cwd=ROOT).returncode:
        raise Failed(failure)


# ---- what the steps decide by (also tested: tests/test_build_game.py) ---------------------
def pick_program(ncas):
    """The program to build from, of [(path, content type, title id)]: the one Program NCA, or
    of two, the update's (its title id ends in 800) - gamefiles.pick_program's rule."""
    import gamefiles
    contents = [{"path": path, "name": os.path.basename(path), "container": None, "content_id": None,
                 "type": "program" if kind == PROGRAM else "other", "title_id": f"{title:016X}",
                 "app": f"{gamefiles.app_of(title):016X}", "kind": gamefiles.kind_of(title), "version": None}
                for path, kind, title in ncas]
    try:
        return gamefiles.pick_program(contents)["path"]
    except gamefiles.Error as e:
        raise Failed(str(e))


def marker_applies(marker):
    """Whether a requirement's environment marker holds here: the python_version comparisons
    requirements.txt uses (`python_version < "3.14"`), joined by `and`; anything else: yes."""
    here = tuple(sys.version_info[:2])
    for clause in marker.split(" and "):
        m = re.match(r'\s*python_version\s*(<=|>=|==|!=|<|>)\s*"(\d+)\.(\d+)"\s*$', clause)
        if not m:
            continue
        op, want = m.group(1), (int(m.group(2)), int(m.group(3)))
        holds = {"<": here < want, "<=": here <= want, ">": here > want, ">=": here >= want,
                 "==": here == want, "!=": here != want}[op]
        if not holds:
            return False
    return True


def dlc_folder(game, rec, contents):
    """Where the DLC is: the game folder's dlc folder; else the game folder itself, when DLC of
    this game is next to the game (an emulator's installed content, loose .nsp files); else None."""
    dlc = os.path.join(game, "dlc")
    if os.path.isdir(dlc):
        return dlc
    return game if any(c["kind"] == "dlc" and c["app"] == rec["app"] for c in contents) else None


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
            if ";" in line and not marker_applies(line.split(";", 1)[1].split("#")[0]):
                continue
            try:
                metadata.version(name)
            except metadata.PackageNotFoundError:
                missing.append(name)
    return missing


# ---- the steps ------------------------------------------------------------------------------
def check_tools():
    if sys.version_info < (3, 10):
        raise Failed(f"this is Python {sys.version.split()[0]}: WindowSmash needs Python 3.10 or newer (setup guide step 3).")
    log(f"  Python {sys.version.split()[0]}: {sys.executable}")
    if LINUX:
        check_linux_tools()
        return
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


LINUX_CHECK = r"""#include <SDL.h>
#include <stdio.h>
#include <vpx/vpx_decoder.h>
extern "C" const char *opus_get_version_string(void);
int main(int argc, char **argv) {
    try { if (argc > 99) throw argc; } catch (int) { return 1; }
    printf("SDL2 %d.%d, %s, %s\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, opus_get_version_string(), vpx_codec_version_str());
    return 0;
}
"""


def linux_toolchain():
    """tools/linux/toolchain.py, or Failed saying what is missing."""
    sys.path.insert(0, os.path.join(TOOLS, "linux"))
    try:
        import toolchain
    except SystemExit as e:
        raise Failed(f"{e}. Run install-windowsmash.sh: it installs them.")
    return toolchain


def check_linux_tools():
    """clang and lld, bison and flex, DXVK and vkd3d's source (install-windowsmash.sh downloads them
    into deps/linux-toolchain), and a test program linked as the game is: C++ with exceptions,
    SDL2, libopus and libvpx."""
    tc = linux_toolchain()
    missing = [n for n in ("bison", "flex") if not shutil.which(n)]
    if missing:
        raise Failed(f"{' and '.join(missing)} not found (vkd3d's shader compiler is made with them). Run "
                     "install-windowsmash.sh: it installs them.")
    parts = {"DXVK": os.path.join(tc.DXVK_LIBS, "libdxvk_d3d11.so.0"), "DXVK's headers": os.path.join(tc.DXVK_INCLUDE, "windows"),
             "vkd3d's source": tc.VKD3D_SRC}
    absent = [name for name, path in parts.items() if not path or not os.path.exists(path)]
    if absent:
        raise Failed(f"{', '.join(absent)} not found in {tc.TOOLCHAIN}. Run install-windowsmash.sh: it downloads them.")
    import install_linux   # (beside toolchain.py)
    gone = [h for h, _ in install_linux.missing_headers(None, os.path.exists)]
    if gone:
        raise Failed(f"{', '.join(gone)} missing from {install_linux.INCLUDE}, and the build needs them (a package can be "
                     "installed without its headers). Run install-windowsmash.sh: it puts back the packages they come with.")
    with tempfile.TemporaryDirectory(prefix="windowsmash-check-") as tmp:
        src, exe = os.path.join(tmp, "check.cpp"), os.path.join(tmp, "check")
        with open(src, "w") as f:
            f.write(LINUX_CHECK)
        r = subprocess.run([tc.CLANG, *tc.TARGET, "-std=gnu++20", "-I/usr/include/SDL2", "-D_REENTRANT", src, "-o", exe,
                            "-fuse-ld=lld", "-no-pie", "-l:libSDL2-2.0.so.0", "-lopus", "-lvpx", "-lstdc++", "-lm", "-lpthread"],
                           capture_output=True, text=True)
        if r.returncode:
            raise Failed(f"{tc.CLANG} could not build a test program with SDL2, libopus and libvpx. Run "
                         "install-windowsmash.sh: it installs what is missing.\n" + (r.stdout + r.stderr)[-2000:])
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            raise Failed(f"the test program built with {tc.CLANG} does not run:\n" + (r.stdout + r.stderr)[-2000:])
    log(f"  clang: {tc.CLANG}; {r.stdout.strip()}")


def install_packages():
    requirements = os.path.join(ROOT, "requirements.txt")
    missing = missing_packages(requirements)
    if not missing:
        log("  already installed")
        return
    run([sys.executable, "-m", "pip", "install", "-r", "requirements.txt"],
        "pip could not install " + ", ".join(missing) + " (see above)."
        + (" On Linux, install-windowsmash.sh installs them in a Python of WindowSmash's own (deps/venv)." if LINUX else ""))
    importlib.invalidate_caches()


FOLDERS = {
    "game": ("Folder with your game files", "~/MyDump" if LINUX else r"D:\MyDump",
             lambda p: None if os.path.isdir(p) else f"There is no folder {p}."),
    "keys": ("Folder with your keys", "~/MyKeys" if LINUX else r"D:\MyKeys",
             lambda p: None if os.path.isfile(os.path.join(p, "prod.keys")) else f"There is no prod.keys in {p}."),
    "firmware": ("Folder with your firmware", "~/MyFirmware" if LINUX else r"D:\MyFirmware",
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
        answer = os.path.expanduser(answer)
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
    """(path, title id, record, scan) of the program to build from, among the game folder's
    files (tools/gamefiles.py: the guide's folder, an emulator's installed-content folder, update
    and DLC .nsp packages)."""
    import gamefiles
    result = gamefiles.scan(game, keys)
    contents, unread = result["contents"], result["unread"]
    for line in gamefiles.summary(result):
        log(f"  {line}")
    try:
        program = gamefiles.pick_program(contents)
    except gamefiles.Error as e:
        why = [f"Looked for the game's program among the {len(contents) + len(unread)} game files in {game}: {e}."]
        if unread:
            why.append("These could not be read (keys missing or too old?):\n    " + "\n    ".join(unread[:8]))
        if not contents and not unread:
            why.append("Copy the game's program .nca there (setup guide step 2).")
        raise Failed("\n  ".join(why))
    title = int(program["title_id"], 16)
    log(f"  program: {gamefiles.where(program)} (title id {title:016X})")
    return program["path"], title, program, result


def nca_ref(rec):
    """nca.py's arguments naming the NCA of a scan record: its file, or its package and --inner."""
    return [rec["path"], "--inner", rec["name"]] if rec["container"] else [rec["path"]]


def game_identity(rec):
    """What the build is of: the game, base or update, and the version (game_source.txt's first line)."""
    return f"{rec['app']} {rec['kind']} {rec['version'] if rec['version'] is not None else '-'}"


def describe_identity(line, display=None):
    """For a person: the version as the game shows it (13.0.5) when known, else the title version."""
    _app, kind, version = (line.split() + ["-", "-", "-"])[:3]
    what = "the game's update" if kind == "update" else "the game"
    if display:
        return f"{display} ({what})"
    return f"{what} (title version v{int(version) >> 16})" if version.isdigit() else what


def same_game(was, now):
    """Whether two game_identity lines name the same build source; a version unknown on either
    side (a renamed file without its metadata) is not held against it."""
    w, n = (was.split() + ["-", "-", "-"])[:3], (now.split() + ["-", "-", "-"])[:3]
    return w[:2] == n[:2] and (w[2] == n[2] or "-" in (w[2], n[2]))


def check_same_game(rec):
    """This folder was set up from one version of the game (dump/game_source.txt); refuse to mix in
    another: its program, data and fighters would no longer match."""
    if not os.path.isfile(SOURCE):
        return
    with open(SOURCE, encoding="utf-8") as f:
        was = f.readline().strip()
        was_display = f.readline().split("\t")[0].strip()
    now = game_identity(rec)
    if was and not same_game(was, now):
        raise Failed(f"This WindowSmash folder was set up from {describe_identity(was, was_display)}, but the game "
                     f"folder now has {describe_identity(now, rec['display_version'])} to build from. One folder holds "
                     "one game version: set up the new one in a new WindowSmash folder, or add it as a version of its "
                     "own (docs/VERSIONS.md).")


def write_game_source(rec):
    with open(SOURCE, "w", encoding="utf-8", newline="\n") as f:
        f.write(game_identity(rec) + "\n")
        f.write(f"{rec['display_version'] or ''}\t{rec['path']}\t{rec['name'] if rec['container'] else ''}\n")


def extract_exefs(rec, keys):
    import gamefiles
    partial = EXEFS + ".partial"
    run([sys.executable, os.path.join("tools", "nca.py"), "exefs", *nca_ref(rec), "--keys", keys,
         "--out", os.path.join("dump", "exefs.partial")],
        f"Could not copy the program out of {gamefiles.where(rec)} (see above).")
    missing = [n for n in EXEFS_FILES if not os.path.isfile(os.path.join(partial, n))]
    if missing:
        raise Failed(f"{gamefiles.where(rec)} has no {', '.join(missing)}: it is not the game's program.")
    os.makedirs(EXEFS, exist_ok=True)
    for n in os.listdir(partial):
        os.replace(os.path.join(partial, n), os.path.join(EXEFS, n))
    os.rmdir(partial)
    write_game_source(rec)


def extract_fonts(firmware, keys):
    """What `nca.py romfs-get <nca> --match .bfttf --flat` gets from each firmware NCA, read in
    this one process; an NCA it cannot read (no RomFS, no key) has no fonts for the game."""
    from nca import Nca, copy_out, romfs_entries
    import gamefiles
    partial = FONTS + ".partial"
    ncas, _nsps = gamefiles.list_ncas(firmware, depth=0)   # (files, or Ryujinx's <id>.nca\00)
    log(f"  reading the {len(ncas)} .nca files in {firmware}")
    for path, _name in ncas:
        nca = None
        try:
            nca = Nca(path, keys)
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
    if os.path.isfile(romfs_bin) and LINUX:   # (Configure-Game-Data.ps1's checks are Windows-only: the format's here)
        from nca import is_plain_romfs
        if not is_plain_romfs(romfs_bin):
            raise Failed(f"{romfs_bin} is not the game's data as a plain RomFS image (unencrypted, with your update "
                         "applied). Remove it, and WindowSmash reads the data from the game's own files instead.")
        os.makedirs(MODULES, exist_ok=True)
        with open(os.path.join(MODULES, "romfs.cfg"), "w", newline="\n") as f:
            f.write(f"{os.path.abspath(romfs_bin)}\n0\n")
        log(f"  {romfs_bin}: the game's data, read in place")
        return
    if os.path.isfile(romfs_bin):
        run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
             os.path.join("tools", "Configure-Game-Data.ps1"), "-NonInteractive", "-RomfsPath", romfs_bin,
             "-FontsDirectory", FONTS],
            f"{romfs_bin} could not be used (see above): it must be the game's data, unencrypted and with "
            "your update applied (setup guide part 2).")
        return
    import gamefiles
    path, _title, program, result = the_program()
    cfg = os.path.join("dump", "modules", "romfs.cfg")
    state = program["romfs"]
    if state == "plain":   # readable in place
        run([sys.executable, os.path.join("tools", "nca.py"), "romfs-cfg", *nca_ref(program), "--keys", keys, "--out", cfg],
            f"The game data inside {gamefiles.where(program)} can't be read in place (see above). Put the game's data in "
            f"{game} as romfs.bin: unencrypted, with your update applied (setup guide part 2).")
    elif state == "encrypted":   # the game (or a release with its update merged in), as the Switch stores it
        plain = decrypt_once(program, keys)
        run([sys.executable, os.path.join("tools", "nca.py"), "romfs-cfg", plain, "--keys", keys, "--out", cfg],
            f"The game data inside {os.path.basename(plain)} can't be read (see above).")
    elif state == "patch":   # an update: its data is a patch over the game's own, applied by update.py
        base = gamefiles.base_program(result["contents"], program)
        if not base:
            raise Failed(f"{gamefiles.where(program)} is the game's update, whose data is a patch over the game's own: "
                         f"WindowSmash also needs the game itself (its program .nca, or its .nsp) in {game}, next to it.")
        base_cfg = os.path.join("dump", "modules", "romfs_base.cfg")
        base_ref = nca_ref(base) if base["romfs"] == "plain" else [decrypt_once(base, keys)]
        run([sys.executable, os.path.join("tools", "nca.py"), "romfs-cfg", *base_ref, "--keys", keys, "--out", base_cfg],
            f"The game data inside {gamefiles.where(base)} can't be read (see above).")
        log("  applying the update over the game's data (a few minutes; every block of the result is checked)")
        inner = ["--inner", program["name"]] if program["container"] else []   # (the package may hold the game too)
        run([sys.executable, os.path.join("tools", "update.py"), "apply", path, *inner, "--base", base_cfg, "--keys", keys,
             "--tree", ROOT],
            "The update could not be applied over the game's data (see above).")
    else:
        raise Failed(f"There is no romfs.bin in {game}, and {gamefiles.where(program)} holds no game data. Put the game's "
                     "data there as romfs.bin: unencrypted, with your update applied (setup guide part 2).")


def decrypt_once(record, keys):
    """The NCA decrypted into dump/modules, for reading in place; kept once it is complete."""
    import gamefiles
    out = os.path.join(MODULES, f"{record['kind']}-{record['title_id'].lower()}.nca")
    if os.path.isfile(out) and os.path.getsize(out) == record["size"]:
        log(f"  {os.path.relpath(out, ROOT)}: already decrypted")
        return out
    log(f"  decrypting {gamefiles.where(record)} into {os.path.relpath(out, ROOT)} ({record['size'] / 2**30:.1f} GB: "
        "some minutes; if it stops, run this again to carry on)")
    run([sys.executable, os.path.join("tools", "nca.py"), "decrypt", *nca_ref(record), "--keys", keys, "--out", out],
        f"Could not decrypt {gamefiles.where(record)} (see above).")
    return out


def write_version():
    """version.txt: the game version the launchers name (tools/pick_version.bat), data.arc's, as
    install_version.py describe writes it for a version's own folder. Once: one folder holds one
    game version. (Only a label: one that can't be read doesn't stop the build.)"""
    path = os.path.join(ROOT, "version.txt")
    if os.path.isfile(path):
        return
    try:
        from arc import Arc
        from nso import Module
        v = Arc(os.path.join(MODULES, "romfs.cfg")).version
        data = f"{v >> 16}.{(v >> 8) & 0xFF}.{v & 0xFF}"
        text = (f"{data}\n# game version of this folder (tools/build_game.py): the version, then what it is made of\n"
                f"main {Module(os.path.join(EXEFS, 'main')).build_id}\ndata.arc {data}\n")
    except Exception as e:
        log(f"  (the game version could not be read for the launchers: {e})")
        return
    with open(path, "w", newline="\n") as f:
        f.write(text)
    log(f"  game version {data} (version.txt)")


def build_is_current():
    """build/ssbu.exe is newer than the runtime, the tools and the modules it is built from, and
    has SDL3.dll beside it if SDL3 is there to build with (on Linux: build-linux/ssbu, also newer
    than the Linux runtime and tools)."""
    if not os.path.isfile(EXE):
        return False
    if not LINUX:
        import cc
        if cc.SDL3_DLL and not os.path.isfile(os.path.join(ROOT, "build", "SDL3.dll")):
            return False
    inputs = [os.path.join(ROOT, "runtime", n) for n in os.listdir(os.path.join(ROOT, "runtime"))]
    inputs += [os.path.join(TOOLS, n) for n in os.listdir(TOOLS) if n.endswith(".py")]
    if LINUX:
        inputs += [os.path.join(ROOT, "runtime", "linux", n) for n in os.listdir(os.path.join(ROOT, "runtime", "linux"))]
        inputs += [os.path.join(TOOLS, "linux", n) for n in os.listdir(os.path.join(TOOLS, "linux")) if n.endswith(".py")]
    inputs += [os.path.join(EXEFS, n) for n in os.listdir(EXEFS)]
    nro = os.path.join(DUMP, "nro")
    inputs += [os.path.join(nro, n) for n in os.listdir(nro) if n.endswith(".nro")] if os.path.isdir(nro) else []
    return os.path.getmtime(EXE) > max(os.path.getmtime(p) for p in inputs if os.path.isfile(p))


def installed_ram_gb():
    if LINUX:
        try:
            return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / (1 << 30)
        except (ValueError, OSError):
            return None
    import ctypes
    kb = ctypes.c_ulonglong()
    if not ctypes.windll.kernel32.GetPhysicallyInstalledSystemMemory(ctypes.byref(kb)):
        return None
    return kb.value / (1 << 20)


def in_use(path):
    """A running program's file can't be opened for writing (on Linux: "text file busy")."""
    try:
        with open(path, "r+b"):
            return False
    except FileNotFoundError:
        return False
    except PermissionError:
        return True
    except OSError as e:
        return e.errno == errno.ETXTBSY


def build(a):
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
    # (a game folder named on the command line is always looked at: it may hold another version now)
    if a.game or not (exefs_done and romfs_done and os.path.exists(aoc)):
        game = ask_folder("game", a.game, saved)
    if missing_fonts(FONTS):
        fonts_from = "game" if game and not missing_fonts(os.path.join(game, "fonts")) else "firmware"
    if game or fonts_from == "firmware":
        keys = ask_folder("keys", a.keys, saved)
    if fonts_from == "firmware":
        firmware = ask_folder("firmware", a.firmware, saved)
    if not keys:
        log("  none needed: the steps that read them are done")

    found = None   # (find_program's answer, once asked; the steps below read it, never set it)

    def the_program():
        nonlocal found
        found = found or find_program(game, keys)
        return found

    log(shown("\n[4/9] Copy the game's program (dump\\exefs)"))
    if exefs_done:
        if game:
            check_same_game(the_program()[2])
        log("  already done")
    else:
        extract_exefs(the_program()[2], keys)

    log(shown("\n[5/9] Take the fonts out of your firmware (dump\\fonts)"))
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

    log(shown("\n[6/9] Connect your game data (dump\\modules\\romfs.cfg)"))
    if romfs_done:
        log("  already done")
    else:
        if os.path.exists(cfg):
            log(f"  {cfg} reads a file that is not there any more: setting it again")
        write_romfs_cfg(game, keys, the_program)

    log(shown("\n[7/9] Turn on your DLC (dump\\modules\\aoc.cfg)"))
    if os.path.exists(aoc):
        log("  already done")
    else:
        _path, title, rec, result = the_program()
        dlc = dlc_folder(game, rec, result["contents"])
        if not dlc:
            log(f"  no dlc folder in {game}, and no DLC among its files: no DLC")
        else:
            run([sys.executable, os.path.join("tools", "nca.py"), "aoc-cfg", dlc, "--keys", keys,
                 "--out", os.path.join("dump", "modules", "aoc.cfg"), "--app", f"{title & ~0xFFF:016X}"],
                f"Could not read the DLC in {dlc} (see above).")

    log(shown("\n[8/9] Take out the fighters (dump\\nro)"))
    run([sys.executable, os.path.join("tools", "arc.py"), "nro"],
        "Could not read the fighters from the game data (see above). Check that the game data from step 6 is "
        "the game's, with your update applied.")
    write_version()

    exe = os.path.relpath(EXE, ROOT)
    log(f"\n[9/9] Build the game ({exe})")
    if a.prepare_only:
        log("  not now (--prepare-only): everything the build needs is ready")
        return
    if build_is_current():
        log(f"  {exe} is newer than everything it is built from: nothing to do (delete it to build again)")
    else:
        if in_use(EXE):
            raise Failed(f"The game is running from {exe}: close it, then run this again.")
        jobs = a.jobs
        if not jobs:
            gb = installed_ram_gb()
            jobs = jobs_for_ram(gb) if gb else 4
            log(f"  {jobs} jobs at a time, for {gb:.0f} GB of RAM" if gb else f"  {jobs} jobs at a time (the RAM size is unknown)")
        if LINUX:
            build_linux_program(jobs)
        else:
            import cc
            if not cc.SDL3_DLL:
                log("  (no SDL3 in deps\\SDL3: Xbox controllers and the keyboard only - setup guide's SDL3 step)")
            log("  the first build takes from 30 minutes to a few hours, later ones minutes; if it stops, run this again to carry on")
            run([sys.executable, os.path.join("tools", "recomp.py"), "all", os.path.join("dump", "exefs"),
                 "--out", "build", "--jobs", str(jobs)],
                "The build failed (see above). If the PC ran out of memory, build with fewer jobs at a time: open "
                "PowerShell in this folder and run  & \".\\launchers\\Build WindowSmash.bat\" --jobs 4")
    if LINUX:
        log(f"\nDone: {exe} is ready. Start the game with ./play-ssbu.sh, or WindowSmash in your applications menu.")
    else:
        log("\nDone: build\\ssbu.exe is ready. Start the game with Play SSBU.bat.")


def build_linux_program(jobs):
    """build-linux/ssbu: the game's C generated into build/, then compiled and linked for Linux."""
    retry = (f"If the PC ran out of memory, build with fewer jobs at a time: bash install-windowsmash.sh --jobs "
             f"{max(1, jobs // 2)}")
    log("  the first build takes one to a few hours, later ones minutes; if it stops, run this again to carry on")
    log("  the game's code, as C:")
    run([sys.executable, os.path.join("tools", "recomp.py"), "all", os.path.join("dump", "exefs"), "--out", "build",
         "--jobs", str(jobs), "--generate-only"],
        "The game's code could not be turned into C (see above). " + retry)
    tc = linux_toolchain()
    if not os.path.isfile(os.path.join(tc.VKD3D_BUILD, "linux", "libvkd3d-shader.a")):
        log("  the shader compiler (vkd3d-shader), once:")
        run([sys.executable, os.path.join("tools", "linux", "build_vkd3d.py"), "linux"],
            "vkd3d-shader could not be built (see above).")
    log("  compiling it for Linux:")
    run([sys.executable, os.path.join("tools", "linux", "build_linux.py"), "--port", ROOT, "--out", "build-linux",
         "--jobs", str(jobs)],
        "The build failed (see above). " + retry)


def main():
    ap = argparse.ArgumentParser(description="Build the game from your own files (launchers\\Build WindowSmash.bat).")
    ap.add_argument("--game", help=r"the folder with the game's files (asked; default D:\MyDump)")
    ap.add_argument("--keys", help=r"the folder with prod.keys and title.keys (asked; default D:\MyKeys)")
    ap.add_argument("--firmware", help=r"the folder with the firmware's .nca files (asked; default D:\MyFirmware)")
    ap.add_argument("--jobs", type=int, help="how many files to compile at a time (default: by the RAM)")
    ap.add_argument("--check", action="store_true", help="only check the tools (step 1) and stop")
    ap.add_argument("--prepare-only", action="store_true", help="do every step but the build itself (9)")
    a = ap.parse_args()
    try:
        if a.check:
            log("[1/9] Check the tools")
            check_tools()
            log("\nThe tools are ready.")
        else:
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
