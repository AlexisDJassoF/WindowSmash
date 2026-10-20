#!/usr/bin/env python3
"""Bundle the current SSBU Windows build. Python 3.10+, standard library only."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import time
import zipfile

CHUNK = 8 * 1024 * 1024
PREFIX = 'SSBU-Portable/'
DEFAULT_SOURCE = Path(__file__).resolve().parent.parent
FONTS = (
    'nintendo_udsg-r_std_003.bfttf', 'nintendo_udsg-r_org_zh-cn_003.bfttf',
    'nintendo_udsg-r_ext_zh-cn_003.bfttf', 'nintendo_udjxh-db_zh-tw_003.bfttf',
    'nintendo_udsg-r_ko_003.bfttf', 'nintendo_ext_003.bfttf',
)
LAUNCHERS = ('Play SSBU.bat', 'launchers/Play SSBU Offline.bat', 'launchers/Host SSBU Netplay.bat',
             'launchers/Join SSBU Netplay.bat')   # (the ones in launchers/ expect to be one folder down)
START_HERE = r'''SSBU PORTABLE - WINDOWS

Extract the ENTIRE ZIP to a writable local folder. Do not play inside the ZIP.
Use NTFS or exFAT, not FAT32. Allow at least 22 GB for extraction plus space for
saves and caches. Keeping both the ZIP and game takes roughly 40 GB.

Run Play SSBU.bat to play. It goes online (below) and plays offline by itself
if the online lobby does not answer; launchers\Play SSBU Offline.bat never goes
online.
No compiler, Python, or separate emulator installation is needed to play this
build. Windows 10/11 x64, an AVX2/FMA/F16C CPU, and a Direct3D 11-capable GPU
are required.

PLAY ONLINE - ONE PLAYER PER PC
Run Play SSBU.bat. The first time, the game picks the name other players see;
press Esc in the game for the settings (your name, a lobby code, controllers,
display). Open Local Wireless in the main-menu sidebar: Create a Room
to host, or pick anyone's room from the Rooms List. Rooms are public to players
of the same game version. For friends only, everyone sets the same lobby code
(settings, Online page; or after the address in online_server.txt).
Allow the game through Windows Firewall if asked. The overlay (F1) shows
"Online: N players, M rooms" when connected.

PRIVATE ROOM - ONE PLAYER PER PC
Host: run launchers\Host SSBU Netplay.bat. Choose your player name and a room name.
Allow the game through Windows Firewall if asked. Give your friend the address
shown by the launcher, netplay_address.txt, or the game's overlay (F2).
Open Local Wireless in the main-menu sidebar, then Create a Room.

Friend: run launchers\Join SSBU Netplay.bat. Enter the host's address, your player name,
and the SAME room name. Open Local Wireless and select the host's room.
The host accepts the challenger. Both PCs must use the same build.

If unreachable, forward UDP port 39420 on the host's router or use a shared
virtual LAN and join the host's VPN IP. Joining players need no port forwarding.
The optional tools\ssbu_relay.py is the online lobby server; it requires Python.
Nintendo's official online service is not used.

CONTROLS
Up to 8 players on Xbox, PlayStation, Switch Pro and most other pads (SDL3);
each pad that connects becomes the next free player. Face buttons follow
Nintendo's physical positions. controllers.ini (written on the first start)
sets the keyboard's player, remaps buttons, rumble strength and dead zones.
Keyboard: arrows = move; Z/X/C/V = A/B/X/Y; A/S = L/R; Q/W = ZL/ZR;
Enter = Plus; Backspace = Minus; I/J/K/L = right stick.
F1 = overlay; F2 = details (lists the pads); F3 = screenshot.
F11 or Alt+Enter = full screen. display.ini (written on the first start) sets
the window (windowed, borderless, fullscreen), its size, and the pacing:
pacing = vrr is best on G-Sync / FreeSync monitors.

Each install starts with a fresh save and generates its own netplay identity.
If launchers\Unlock All Fighters.bat is included, first start the game, reach the main menu,
and close it. Run that button to back up and unlock your own save; no Python is
needed. DLC is enabled by the included aoc.cfg when present. This 11.0.0 build
has DLC through Pyra/Mythra; Kazuya and Sora require a newer game version.
Do not copy netplay_id.txt between PCs. Saves are under save\.
Check build\play.log if launch fails. Initial shader compilation can stutter.
A crash or a freeze (no picture for 5 s) writes a report and a minidump
into the reports folder: send those when you report a problem.
This is a snapshot of an existing port; packaging verification does not test
gameplay or a complete network match on another PC.
'''


def log(message: str) -> None:
    print(message, flush=True)


def fingerprint(path: Path) -> tuple[int, int, int, int]:
    stat = path.stat()
    return stat.st_size, stat.st_mtime_ns, stat.st_ino, stat.st_dev


@dataclass(frozen=True)
class Payload:
    name: str
    path: Path | None = None
    data: bytes | None = None
    offset: int = 0
    size: int = 0
    signature: tuple[int, int, int, int] | None = None
    stored: bool = False


def file_payload(name: str, path: Path, offset: int = 0, stored: bool = False) -> Payload:
    if not path.is_file():
        raise ValueError(f'Missing required file: {path}')
    sig = fingerprint(path)
    if offset < 0 or offset >= sig[0]:
        raise ValueError(f'Empty file or invalid offset: {path}')
    return Payload(name, path=path, offset=offset, size=sig[0] - offset,
                   signature=sig, stored=stored)


def text_payload(name: str, text: str) -> Payload:
    data = text.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8')
    return Payload(name, data=data, size=len(data))


def build_plan(source: Path, shader_cache: bool = True) -> list[Payload]:
    payload = [file_payload('build/ssbu.exe', source / 'build/ssbu.exe')]
    modules = source / 'dump/modules'
    images = sorted(modules.glob('*.reloc.bin'))
    names = {p.name.removesuffix('.reloc.bin') for p in images}
    expected = {'main', 'sdk', 'subsdk0'}
    table = source / 'build/rt/nro_table.c'
    if table.is_file():
        expected.update(re.findall(r'extern\s+const\s+RtModule\s+rt_module_(\w+)\s*;', table.read_text(encoding='utf-8')))
    missing = sorted(expected - names)
    if missing:
        raise ValueError('Missing runtime module images: ' + ', '.join(missing))
    payload.extend(file_payload('dump/modules/' + p.name, p) for p in images)
    aoc = modules / 'aoc.cfg'
    if aoc.is_file():
        payload.append(file_payload('dump/modules/aoc.cfg', aoc))
    payload.extend(file_payload('dump/fonts/' + name, source / 'dump/fonts' / name) for name in FONTS)
    for path in sorted((source / 'build').glob('*.dll')):
        payload.append(file_payload('build/' + path.name, path))
    sdl_licence = source / 'deps/SDL3/LICENSE.txt'   # (the zlib licence of build/SDL3.dll)
    if (source / 'build/SDL3.dll').is_file() and sdl_licence.is_file():
        payload.append(file_payload('build/SDL3-LICENSE.txt', sdl_licence))
    if shader_cache:
        # only the runtime's current cache version (older files are never read), and only what
        # the game has used: tools/shader_aot.py's files stay dated 2001 until then
        header = source / 'runtime/gpu_shader_cache.h'
        version = re.search(r'#define SHADER_CACHE_VERSION (\d+)', header.read_text(encoding='utf-8')) if header.is_file() else None
        pattern = f'v{version.group(1)}_*.dxbc' if version else '*.dxbc'
        played_after = datetime(2002, 1, 1, tzinfo=timezone.utc).timestamp()
        for path in sorted((source / 'dump/shadercache').glob(pattern)):
            if path.stat().st_mtime >= played_after:
                payload.append(file_payload('dump/shadercache/' + path.name, path))
    relay = source / 'tools/ssbu_relay.py'
    if relay.is_file():
        payload.append(file_payload('tools/ssbu_relay.py', relay))
    server = source / 'online_server.txt'
    if server.is_file():
        payload.append(text_payload('online_server.txt', server.read_text(encoding='utf-8-sig')))
    # (every launcher picks the game version through tools/pick_version.bat)
    for name in ('tools/pick_version.bat', 'tools/unlock_all.py', 'tools/Unlock-All-Fighters.ps1', 'launchers/Unlock All Fighters.bat'):
        path = source / name
        if path.is_file():
            payload.append(file_payload(name, path))

    config = (modules / 'romfs.cfg').read_text(encoding='utf-8-sig').splitlines()
    if len(config) < 2 or not config[0].strip():
        raise ValueError('romfs.cfg must contain a data path and a byte offset')
    game_data = Path(config[0])
    if not game_data.is_absolute():
        game_data = source / game_data  # Runtime resolves paths from the port root.
    offset = int(config[1].strip())
    rom = file_payload('data/romfs.bin', game_data, offset, stored=True)
    with game_data.open('rb') as file:
        file.seek(offset)
        header = file.read(80)
    if len(header) != 80:
        raise ValueError('RomFS header is truncated')
    fields = struct.unpack('<10Q', header)
    if fields[0] != 80 or any(fields[i] + fields[i + 1] > rom.size for i in (1, 3, 5, 7)) or fields[9] > rom.size:
        raise ValueError('Invalid or encrypted RomFS at the configured offset')
    payload.append(rom)
    payload.append(text_payload('dump/modules/romfs.cfg', 'data/romfs.bin\n0\n'))
    for name in LAUNCHERS:
        path = source / name
        # Retain current launch behavior, while isolating saves, modes and scripted input.
        reset = '@echo off\nsetlocal\n'
        for key in ('SSBU_DATA_ROOT', 'SSBU_INPUT', 'SSBU_NET_HOST', 'SSBU_NET_RELAY', 'SSBU_NET_BIND', 'SSBU_NET_ROOM', 'SSBU_NET_NAME'):
            reset += f'set "{key}="\n'
        script = path.read_text(encoding='utf-8-sig')
        script = script.replace('Online ^> Local Wireless', 'Local Wireless')
        payload.append(text_payload(name, reset + script))
    payload.append(text_payload('START HERE.txt', START_HERE))
    all_names = [item.name.casefold() for item in payload]
    if len(all_names) != len(set(all_names)):
        raise ValueError('Duplicate archive paths')
    return payload


def check_unchanged(item: Payload) -> None:
    if item.path is not None and fingerprint(item.path) != item.signature:
        raise RuntimeError(f'Source changed while bundling; finish the build and retry: {item.path}')


def write_payload(archive: zipfile.ZipFile, item: Payload) -> dict:
    check_unchanged(item)
    info = zipfile.ZipInfo(PREFIX + item.name, datetime.now().timetuple()[:6])
    info.compress_type = zipfile.ZIP_STORED if item.stored else zipfile.ZIP_DEFLATED
    info.external_attr = 0o100644 << 16
    info.file_size = item.size
    sha = hashlib.sha256()
    if item.data is not None:
        archive.writestr(info, item.data, compresslevel=1)
        sha.update(item.data)
    else:
        done = 0
        report_at = time.monotonic() + 15
        with item.path.open('rb') as source, archive.open(info, 'w', force_zip64=True) as target:
            source.seek(item.offset)
            while done < item.size:
                block = source.read(min(CHUNK, item.size - done))
                if not block:
                    raise RuntimeError(f'Source was truncated: {item.path}')
                target.write(block)
                sha.update(block)
                done += len(block)
                if time.monotonic() >= report_at:
                    log(f'  {item.name}: {done / item.size:.0%}')
                    report_at = time.monotonic() + 15
        check_unchanged(item)
    return {'path': item.name, 'bytes': item.size, 'sha256': sha.hexdigest()}


def verify_archive(path: Path, entries: list[dict], manifest: bytes) -> None:
    expected = {PREFIX + item['path']: item for item in entries}
    expected[PREFIX + 'manifest.json'] = {
        'bytes': len(manifest), 'sha256': hashlib.sha256(manifest).hexdigest(),
    }
    with zipfile.ZipFile(path) as archive:
        if len(archive.infolist()) != len(expected) or set(archive.namelist()) != set(expected):
            raise RuntimeError('Archive file list does not match the manifest')
        for info in archive.infolist():
            entry = expected[info.filename]
            sha = hashlib.sha256()
            with archive.open(info) as file:
                while block := file.read(CHUNK):
                    sha.update(block)  # Reading to EOF also verifies ZIP CRC.
            if info.file_size != entry['bytes'] or sha.hexdigest() != entry['sha256']:
                raise RuntimeError(f'Archive integrity check failed: {info.filename}')


def bundle(source: Path, destination: Path, payload: list[Payload]) -> Path:
    destination.mkdir(parents=True, exist_ok=True)
    estimate = int(sum(item.size for item in payload) * 1.01) + 64 * 1024 * 1024
    free = shutil.disk_usage(destination).free
    if free < estimate:
        raise ValueError(f'Insufficient disk space: need about {estimate / 1e9:.2f} GB; {free / 1e9:.2f} GB free')
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    final = destination / f'SSBU-Portable-Windows-{stamp}.zip'
    partial = final.with_name(final.name + '.partial')
    checksum_path = final.with_name(final.name + '.sha256')
    checksum_partial = final.with_name(final.name + '.sha256.partial')
    if any(path.exists() for path in (final, partial, checksum_path, checksum_partial)):
        raise FileExistsError('Bundle output already exists; retry to create a new timestamp')
    log(f'Writing {partial}')
    try:
        with zipfile.ZipFile(partial, 'x', compression=zipfile.ZIP_DEFLATED,
                             compresslevel=1, allowZip64=True) as archive:
            entries = [write_payload(archive, item) for item in payload]
            for item in payload:
                check_unchanged(item)
            manifest = (json.dumps({
                'format': 1, 'created_utc': datetime.now(timezone.utc).isoformat(),
                'build': 'build/ssbu.exe',
                'build_modified_utc': datetime.fromtimestamp((source / 'build/ssbu.exe').stat().st_mtime, timezone.utc).isoformat(),
                'files': entries,
            }, indent=2) + '\n').encode('utf-8')
            archive.writestr(PREFIX + 'manifest.json', manifest)
        log('Verifying every file: ZIP CRC, length and SHA-256...')
        verify_archive(partial, entries, manifest)
        log('Writing archive SHA-256 checksum...')
        sha = hashlib.sha256()
        with partial.open('rb') as file:
            while block := file.read(CHUNK):
                sha.update(block)
        with checksum_partial.open('x', encoding='ascii', newline='\n') as file:
            file.write(f'{sha.hexdigest()}  {final.name}\n')
        # Windows rename refuses existing destinations. Timestamped names retain old bundles.
        partial.rename(final)
        checksum_partial.rename(checksum_path)
    except BaseException:
        log(f'Bundling did not complete. Any unfinished output remains marked .partial in {destination}')
        raise
    log(f'Created: {final}\nSize: {final.stat().st_size / 1e9:.2f} GB\nChecksum: {checksum_path}')
    return final


def default_source() -> Path:
    script = Path(__file__).resolve()
    for candidate in (script.parent.parent, script.parent):
        if (candidate / 'build/ssbu.exe').is_file():
            return candidate
    return DEFAULT_SOURCE


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=default_source(), help='Port root (auto-detected when installed in tools/)')
    parser.add_argument('--output-dir', type=Path, help='Output directory (default: SOURCE/dist/portable)')
    parser.add_argument('--no-shader-cache', action='store_true', help='Let the recipient generate a fresh shader cache')
    parser.add_argument('--dry-run', action='store_true', help='Validate inputs and report size without writing a bundle')
    args = parser.parse_args(argv)
    source = args.source.expanduser().resolve()
    destination = (args.output_dir or source / 'dist/portable').expanduser().resolve()
    try:
        payload = build_plan(source, shader_cache=not args.no_shader_cache)
        total = sum(item.size for item in payload)
        log(f'Build: {source / "build/ssbu.exe"}')
        log(f'Output: {destination}\nPayload: {len(payload)} files, {total / 1e9:.2f} GB before ZIP compression')
        log('Excludes saves, keys, netplay identities/addresses, logs, screenshots and build intermediates.')
        if args.dry_run:
            log('Input validation passed. Dry run: no files written.')
        else:
            bundle(source, destination, payload)
        return 0
    except (OSError, ValueError, RuntimeError, zipfile.BadZipFile) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print('Cancelled. Any .partial output is unfinished.', file=sys.stderr)
        return 130


if __name__ == '__main__':
    raise SystemExit(main())
