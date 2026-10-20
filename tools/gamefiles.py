#!/usr/bin/env python3
"""What a folder of game files holds, for the build and the installer: the game's NCAs - which
one is the program, the base, the update, the DLC - and the version.

Usage:
  gamefiles.py <folder> --keys <dir> [--json]

The folder can be the setup guide's (program.nca and friends: docs/BUILDING.md) or an
emulator's installed-content folder - yuzu's nand\\user\\Contents\\registered, Ryujinx's
bis\\user\\Contents\\registered - where every content is <content id>.nca, as a file or as a
folder holding the file as 00, next to other games' contents, which are ignored. Folders one
level down are read too (the guide's dlc\\, the emulators' numbered folders), and packages
anywhere in them: the game as a game card dump (.xci) or an .nsp, its update and DLC as dumped
(.nsp). tools/build_game.py reads an NCA inside a package in place with nca.py --inner, and
tools/update.py applies an update's data over the base's.

Title ids: an application's ends in 000, its update's in 800, and its add-on contents' are the
application's + 0x1000 + the DLC's index (Super Smash Bros. Ultimate: 01006A800016E000,
01006A800016E800, 01006A800016F001 and so on). Those are the metadata's (.cnmt); the NCAs of an
update carry the application's title id in their own headers, so what an NCA is for comes from
the metadata that lists it (apply_metadata), or from its data being a patch.
"""
import argparse
import glob
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nca import Nca, nsp_ncas, pfs0_entries, romfs_entries  # noqa: E402

APP_ID = 0x01006A800016E000   # Super Smash Bros. Ultimate
CONTENT_TYPES = {0: "program", 1: "meta", 2: "control", 3: "manual", 4: "data", 5: "publicdata"}
ROMFS_STATES = {1: "plain", 3: "encrypted", 4: "patch"}   # an NCA section's encryption type


class Error(Exception):
    """Something about the folder that stops the build; the message says what."""


YUZU_LIKE = ("yuzu", "eden", "sudachi", "citron", "suyu")   # yuzu and the forks with its layout


def emulator_folders(env=os.environ, home=None, find=glob.glob):
    """Where the emulators keep keys, firmware and installed games on Linux, most likely first:
    {"keys": [...], "firmware": [...], "game": [...]} (the folders may not exist). yuzu and its
    forks keep theirs in the XDG data folder, Ryujinx in the XDG config folder; installed as a
    Flatpak, each in its own ~/.var/app/<id>/data or config. (On Windows the installer has the
    same list for %APPDATA%: tools/install_windowsmash.ps1 Get-FolderCandidates.)"""
    home = home or os.path.expanduser("~")
    data = [env.get("XDG_DATA_HOME") or os.path.join(home, ".local", "share")]
    config = [env.get("XDG_CONFIG_HOME") or os.path.join(home, ".config")]
    data += sorted(find(os.path.join(home, ".var", "app", "*", "data")))
    config += sorted(find(os.path.join(home, ".var", "app", "*", "config")))
    found = {"keys": [], "firmware": [], "game": []}
    for root in data:
        for name in YUZU_LIKE:
            emu = os.path.join(root, name)
            found["keys"].append(os.path.join(emu, "keys"))
            found["firmware"].append(os.path.join(emu, "nand", "system", "Contents", "registered"))
            found["game"].append(os.path.join(emu, "nand", "user", "Contents", "registered"))
            found["game"].append(os.path.join(emu, "sdmc", "Nintendo", "Contents", "registered"))
    for root in config:
        emu = os.path.join(root, "Ryujinx")
        found["keys"].append(os.path.join(emu, "system"))
        found["firmware"].append(os.path.join(emu, "bis", "system", "Contents", "registered"))
        found["game"].append(os.path.join(emu, "bis", "user", "Contents", "registered"))
        found["game"].append(os.path.join(emu, "sdcard", "Nintendo", "Contents", "registered"))
    found["keys"].append(os.path.join(home, ".switch"))
    return found


# ---- pure: what a title id says ---------------------------------------------------------------
def kind_of(title_id):
    """base (the application), update, or dlc."""
    low = title_id & 0xFFF
    return "base" if low == 0 else "update" if low == 0x800 else "dlc"


def app_of(title_id):
    """The application a title id belongs to."""
    kind = kind_of(title_id)
    return title_id if kind == "base" else title_id - 0x800 if kind == "update" else (title_id & ~0xFFF) - 0x1000


def version_string(version):
    """A title version as the emulators show it: the number, and its release part (v<n>)."""
    return f"v{version >> 16}"


# ---- the folder's files -------------------------------------------------------------------------
def list_ncas(folder, depth=1):
    """[(path, name)] of the NCAs in a folder and `depth` levels of subfolders: <name>.nca as a
    file, or a folder <name>.nca holding the file as 00 (Ryujinx); .nsp and .xci packages
    likewise, as (path, name) in the second list."""
    ncas, nsps = [], []

    def visit(d, level):
        try:
            names = sorted(os.listdir(d))
        except OSError:
            return
        for n in names:
            p = os.path.join(d, n)
            low = n.lower()
            if low.endswith(".nca"):
                if os.path.isfile(p):
                    ncas.append((p, n))
                elif os.path.isfile(os.path.join(p, "00")):
                    ncas.append((os.path.join(p, "00"), n))
            elif low.endswith((".nsp", ".xci")) and os.path.isfile(p):
                nsps.append((p, n))
            elif os.path.isdir(p) and level < depth:
                visit(p, level + 1)

    visit(folder, 0)
    return ncas, nsps


def cnmt_info(nca):
    """(title id, version, kind, [content ids]) from a Meta NCA's .cnmt, or None."""
    sec = nca.section(1)
    if sec is None:
        return None
    for name, off, size in pfs0_entries(sec):
        if not name.endswith(".cnmt"):
            continue
        cnmt = sec.read(off, size)
        title, version, kind = struct.unpack_from("<QIB", cnmt, 0)
        ext_size, count = struct.unpack_from("<HH", cnmt, 0x0E)
        ids = []
        for i in range(count):
            rec = 0x20 + ext_size + i * 0x38
            ids.append(cnmt[rec + 0x20:rec + 0x30].hex())
        return title, version, kind, ids
    return None


def nacp_info(nca):
    """(name, display version) from a Control NCA's control.nacp, or None."""
    sec = nca.section(0)
    if sec is None:
        return None
    for path, off, _size in romfs_entries(sec):
        if path == "control.nacp":
            name = sec.read(off, 0x200).split(b"\0")[0].decode("utf-8", "replace")
            display = sec.read(off + 0x3060, 0x10).split(b"\0")[0].decode("utf-8", "replace")
            return name, display
    return None


def describe_nca(nca, path, name, container=None):
    """A record for one NCA: its type, title id, what it is for the game, and for a program its
    RomFS's state (plain: readable in place; encrypted; patch: an update over the base)."""
    rec = {"path": path, "name": name, "container": container, "type": CONTENT_TYPES.get(nca.content_type, str(nca.content_type)),
           "title_id": f"{nca.title_id:016X}", "app": f"{app_of(nca.title_id):016X}", "kind": kind_of(nca.title_id),
           "content_id": name[:32].lower() if len(name) >= 36 and name[32:].lower() in (".nca", ".cnmt.nca") else None,
           "size": nca.size, "version": None, "display_version": None, "game": None, "romfs": None}
    if nca.content_type == 0:
        sec = nca.section(0)
        rec["romfs"] = None if sec is None else "plain" if nca.plaintext else ROMFS_STATES.get(sec.enc_type, "encrypted")
        if rec["romfs"] == "patch":   # (an update's program carries the game's own title id: its data says what it is)
            rec["kind"] = "update"
    return rec


META_KINDS = {0x80: "base", 0x81: "update", 0x82: "dlc"}   # a .cnmt's content meta type


def apply_metadata(contents, metas):
    """What the metadata (.cnmt) says about each content, onto the records: its kind and version.
    `metas` are (title id, version, meta type, [content ids]). An NCA's own header is not enough:
    an update's program and control data carry the game's title id, not the update's; only the
    update's metadata lists them. A record without a content id (a renamed file) keeps what its
    header says, its version then from the metadata of its title. Then each program gets the
    display version of the control data of the same game and kind."""
    by_id, by_title = {}, {}
    for title, version, mtype, ids in metas:
        for cid in ids:
            by_id[cid] = (version, META_KINDS.get(mtype))
        by_title[title] = max(by_title.get(title, 0), version)
    for rec in contents:
        known = by_id.get(rec["content_id"]) if rec["content_id"] else None
        if known:
            rec["version"] = known[0] if rec["version"] is None else rec["version"]
            if known[1] and rec["type"] != "meta":
                rec["kind"] = known[1]
        elif rec["version"] is None and rec["kind"] != "update":
            rec["version"] = by_title.get(int(rec["title_id"], 16))
    controls = {}
    for rec in contents:
        if rec["type"] == "control" and rec["display_version"]:
            controls.setdefault((rec["app"], rec["kind"]), (rec["game"], rec["display_version"]))
    for rec in contents:
        if rec["type"] != "control" and rec["display_version"] is None and (rec["app"], rec["kind"]) in controls:
            rec["game"], rec["display_version"] = controls[(rec["app"], rec["kind"])]
    return contents


def scan(folder, keys, depth=1):
    """{"contents": [records], "unread": ["name: why"]} for the folder's NCAs and packages."""
    contents, unread = [], []
    ncas, nsps = list_ncas(folder, depth)
    opened = []
    for path, name in ncas:
        try:
            nca = Nca(path, keys)
        except (SystemExit, OSError, ValueError, struct.error) as e:   # (cut off, or under keys we lack)
            unread.append(f"{name}: {e}")
            continue
        opened.append((nca, path, name, None))
    for path, name in nsps:
        try:
            for inner, nca in nsp_ncas(path, keys):
                opened.append((nca, path, inner, name))
        except (SystemExit, OSError, ValueError, struct.error) as e:
            unread.append(f"{name}: {e}")
    metas = []
    for nca, path, name, container in opened:
        try:
            rec = describe_nca(nca, path, name, container)
            if nca.content_type == 1:
                info = cnmt_info(nca)
                if info:
                    title, version, mtype, ids = info
                    rec["version"] = version
                    rec["kind"] = META_KINDS.get(mtype, rec["kind"])
                    metas.append((title, version, mtype, ids))
            elif nca.content_type == 2:
                info = nacp_info(nca)
                if info:
                    rec["game"], rec["display_version"] = info
            contents.append(rec)
        except (SystemExit, OSError, ValueError, struct.error) as e:
            unread.append(f"{name}: {e}")
        finally:
            nca.f.close()
    apply_metadata(contents, metas)
    return {"folder": os.path.abspath(folder), "contents": contents, "unread": unread}


# ---- pure: what to build from -------------------------------------------------------------------
def pick_program(contents, app=APP_ID):
    """The program record to build from, among a scan's contents: a Program NCA, as a file of its
    own or inside a package (.nsp); of the game's (`app`) when the folder holds several games';
    the update's rather than the base's; of several updates, the newest the metadata knows; of
    the same program twice, the file of its own; else the choice must be made by hand."""
    programs = [c for c in contents if c["type"] == "program"]
    if not programs:
        raise Error("there is no Program NCA among them")
    ours = [c for c in programs if int(c["app"], 16) == app]
    if ours:
        programs = ours
    elif len({c["app"] for c in programs}) > 1:
        raise Error("they are the programs of several games (" + ", ".join(sorted({c["app"] for c in programs}))
                    + "), none of them Super Smash Bros. Ultimate's")
    updates = [c for c in programs if c["kind"] == "update"]
    if updates:
        programs = updates
        if len(updates) > 1 and all(c["version"] is not None for c in updates):
            newest = max(c["version"] for c in updates)
            programs = [c for c in updates if c["version"] == newest]
    if len(programs) > 1:   # the same program twice: one content id, or one known version
        ids = {c["content_id"] for c in programs}
        versions = {(c["kind"], c["version"]) for c in programs}
        if (len(ids) == 1 and None not in ids) or (len(versions) == 1 and programs[0]["version"] is not None):
            programs = sorted(programs, key=lambda c: c["container"] is not None)[:1]
    if len(programs) > 1:
        raise Error("there are several Program NCAs: " + ", ".join(where(c) for c in programs)
                    + ". Keep only the one of the game version to build (setup guide step 2).")
    return programs[0]


def where(rec):
    """A record's file, for a person: the NCA's name, and the package it is in."""
    return f"{rec['name']} in {rec['container']}" if rec["container"] else rec["name"]


def base_program(contents, program):
    """The base's Program NCA for an update's (a file of its own rather than one in a package), or None."""
    bases = [c for c in contents if c["type"] == "program" and c["kind"] == "base" and c["app"] == program["app"]]
    bases.sort(key=lambda c: c["container"] is not None)
    return bases[0] if bases else None


def summary(result, app=APP_ID):
    """Lines for a person: the game's contents, then what else the folder holds."""
    contents = result["contents"]
    lines = []
    ours = [c for c in contents if int(c["app"], 16) == app]
    others = {c["app"] for c in contents if int(c["app"], 16) != app}
    if not contents:
        lines.append("No game files (.nca, .nsp or .xci) found here.")
    for c in ours:
        if c["type"] != "program":
            continue
        what = "the game (base)" if c["kind"] == "base" else "the game's update"
        version = c["display_version"] or (version_string(c["version"]) if c["version"] is not None else "version unknown")
        where = f"{c['name']} in {c['container']}" if c["container"] else c["name"]
        state = {"plain": "readable in place", "encrypted": "encrypted, will be decrypted",
                 "patch": "a patch over the base"}.get(c["romfs"], "no game data inside")
        lines.append(f"{what}: {version} - {where} ({state})")
    dlc = {c["title_id"] for c in ours if c["kind"] == "dlc"}
    if dlc:
        lines.append(f"DLC: {len(dlc)} add-on content{'s' if len(dlc) != 1 else ''}")
    if not ours and contents:
        lines.append("None of them is Super Smash Bros. Ultimate's.")
    if others:
        lines.append(f"({len(others)} other game{'s' if len(others) != 1 else ''} here, ignored)")
    if result["unread"]:
        n = len(result["unread"])
        under = sum(1 for u in result["unread"] if "title key" in u)
        if under == n:
            lines.append(f"({n} file{'s' if n != 1 else ''} under title keys that are not in title.keys, skipped)")
        else:
            lines.append(f"({n} file{'s' if n != 1 else ''} could not be read: keys missing or too old?)")
    return lines


def main():
    ap = argparse.ArgumentParser(description="What a folder of game files holds.")
    ap.add_argument("folder")
    ap.add_argument("--keys", required=True, help="folder with prod.keys and title.keys")
    ap.add_argument("--json", action="store_true", help="the scan as JSON, with the program to build from under 'program'")
    a = ap.parse_args()
    if not os.path.isdir(a.folder):
        raise SystemExit(f"there is no folder {a.folder}")
    result = scan(a.folder, a.keys)
    try:
        program = pick_program(result["contents"])
        problem = None
    except Error as e:
        program, problem = None, str(e)
    result["program"] = program
    result["problem"] = problem
    result["summary"] = summary(result)
    if a.json:
        print(json.dumps(result, indent=1))
    else:
        for line in result["summary"]:
            print(line)
        if program:
            print(f"build from: {program['path']}")
        elif problem:
            print(f"problem: {problem}")
    return 0 if program else 1


if __name__ == "__main__":
    sys.exit(main())
