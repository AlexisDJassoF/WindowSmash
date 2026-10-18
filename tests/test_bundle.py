import contextlib
from datetime import datetime, timezone
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile

sys.dont_write_bytecode = True
WORK = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('bundle_ssbu', WORK.parent / 'tools/bundle_ssbu.py')
bundler = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = bundler
spec.loader.exec_module(bundler)


class BundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='bundle-test-', dir=WORK)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'port with spaces'
        self.output = Path(self.temp.name) / 'bundles'
        for name, data in {
            'build/ssbu.exe': b'MZ-current-build',
            'build/example.dll': b'runtime-dll',
            'build/rt/nro_table.c': b'extern const RtModule rt_module_fighter;',
            'dump/modules/main.reloc.bin': b'main',
            'dump/modules/sdk.reloc.bin': b'sdk',
            'dump/modules/subsdk0.reloc.bin': b'subsdk0',
            'dump/modules/fighter.reloc.bin': b'fighter',
            'dump/modules/romfs.cfg': b'assets/current.nca\n128\n',
            'dump/modules/aoc.cfg': b'1 01006A800016F001\n',
            'assets/current.nca': b'X' * 128 + struct.pack('<10Q', 80, 80, 0, 80, 0, 80, 0, 80, 0, 80) + b'GAME-DATA',
            'dump/shadercache/test.dxbc': b'shader',
            'tools/ssbu_relay.py': b'# optional relay',
            'tools/unlock_all.py': b'# optional unlock tool',
            'tools/Unlock-All-Fighters.ps1': b'# optional no-Python unlock tool',
            'Unlock All Fighters.bat': b'@echo off\n',
            'save/private.dat': b'PRIVATE',
            'prod.keys': b'PRIVATE',
            'netplay_id.txt': b'PRIVATE',
            'netplay_address.txt': b'PRIVATE',
            'build/play.log': b'PRIVATE',
            'build/ssbu_prev.exe': b'OLD BUILD',
            'build/private.obj': b'PRIVATE',
        }.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        for font in bundler.FONTS:
            path = self.root / 'dump/fonts' / font
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'font')
        for name in bundler.LAUNCHERS:
            (self.root / name).write_text('@echo off\ncd /d "%~dp0"\nrem current launcher\n')

    def package(self):
        with contextlib.redirect_stdout(io.StringIO()):
            return bundler.bundle(self.root, self.output, bundler.build_plan(self.root))

    def test_portable_payload_integrity_and_private_exclusions(self):
        path = self.package()
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
            self.assertEqual(archive.read('SSBU-Portable/build/ssbu.exe'), b'MZ-current-build')
            self.assertEqual(archive.read('SSBU-Portable/data/romfs.bin'), (self.root / 'assets/current.nca').read_bytes()[128:])
            self.assertEqual(archive.read('SSBU-Portable/dump/modules/romfs.cfg'), b'data/romfs.bin\r\n0\r\n')
            self.assertEqual(archive.getinfo('SSBU-Portable/data/romfs.bin').compress_type, zipfile.ZIP_STORED)
            for forbidden in ('save/', 'prod.keys', 'netplay_id.txt', 'netplay_address.txt', 'play.log', 'ssbu_prev.exe', 'private.obj'):
                self.assertFalse(any(forbidden in name for name in names))
            self.assertIn('SSBU-Portable/build/example.dll', names)
            self.assertIn('SSBU-Portable/dump/modules/aoc.cfg', names)
            self.assertIn('SSBU-Portable/tools/unlock_all.py', names)
            self.assertIn('SSBU-Portable/tools/Unlock-All-Fighters.ps1', names)
            self.assertIn('SSBU-Portable/Unlock All Fighters.bat', names)
            for launcher in bundler.LAUNCHERS:
                text = archive.read('SSBU-Portable/' + launcher).decode()
                self.assertIn('set "SSBU_NET_HOST="', text)
                self.assertIn('set "SSBU_NET_RELAY="', text)
                self.assertIn('rem current launcher', text)
            manifest = archive.read('SSBU-Portable/manifest.json')
            entries = json.loads(manifest)['files']
        bundler.verify_archive(path, entries, manifest)
        self.assertTrue(path.with_name(path.name + '.sha256').is_file())
        self.assertFalse(list(self.output.glob('*.partial')))

    def test_latest_canonical_build_and_retains_old_bundle(self):
        first = self.package()
        (self.root / 'build/ssbu.exe').write_bytes(b'MZ-next-build')
        second = self.package()
        self.assertNotEqual(first, second)
        with zipfile.ZipFile(first) as old, zipfile.ZipFile(second) as new:
            self.assertEqual(old.read('SSBU-Portable/build/ssbu.exe'), b'MZ-current-build')
            self.assertEqual(new.read('SSBU-Portable/build/ssbu.exe'), b'MZ-next-build')

    def test_missing_compiled_module_rejected(self):
        (self.root / 'build/rt/nro_table.c').write_text('extern const RtModule rt_module_missing;')
        with self.assertRaisesRegex(ValueError, 'missing'):
            bundler.build_plan(self.root)

    def test_bad_romfs_offset_rejected(self):
        (self.root / 'dump/modules/romfs.cfg').write_text('assets/current.nca\n0\n')
        with self.assertRaisesRegex(ValueError, 'Invalid or encrypted RomFS'):
            bundler.build_plan(self.root)

    def test_changed_build_rejected(self):
        plan = bundler.build_plan(self.root)
        (self.root / 'build/ssbu.exe').write_bytes(b'MZ-rebuilt-with-new-content')
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(RuntimeError, 'Source changed'):
            bundler.bundle(self.root, self.output, plan)
        self.assertFalse(list(self.output.glob('*.zip')))

    def test_no_shader_cache_and_no_writes_on_dry_run(self):
        plan = bundler.build_plan(self.root, shader_cache=False)
        self.assertFalse(any(item.name.endswith('.dxbc') for item in plan))
        with contextlib.redirect_stdout(io.StringIO()):
            status = bundler.main(['--source', str(self.root), '--output-dir', str(self.output), '--dry-run'])
        self.assertEqual(status, 0)
        self.assertFalse(self.output.exists())

    def test_sdl3_licence_ships_with_the_dll(self):
        licence = self.root / 'deps/SDL3/LICENSE.txt'
        licence.parent.mkdir(parents=True)
        licence.write_bytes(b'zlib licence text')
        names = [item.name for item in bundler.build_plan(self.root)]
        self.assertNotIn('build/SDL3-LICENSE.txt', names)   # no SDL3.dll in this build
        (self.root / 'build/SDL3.dll').write_bytes(b'MZ-sdl3')
        path = self.package()
        with zipfile.ZipFile(path) as archive:
            self.assertEqual(archive.read('SSBU-Portable/build/SDL3.dll'), b'MZ-sdl3')
            self.assertEqual(archive.read('SSBU-Portable/build/SDL3-LICENSE.txt'), b'zlib licence text')
            self.assertFalse(any('deps/' in name for name in archive.namelist()))

    def test_shader_cache_ships_only_played_files_of_the_current_version(self):
        header = self.root / 'runtime/gpu_shader_cache.h'
        header.parent.mkdir(parents=True)
        header.write_text('#define SHADER_CACHE_VERSION 7\n')
        cache = self.root / 'dump/shadercache'
        for name in ('v7_played.dxbc', 'v7_ahead_of_time.dxbc', 'v6_old.dxbc'):
            (cache / name).write_bytes(b'shader')
        unplayed = datetime(2001, 6, 1, tzinfo=timezone.utc).timestamp()   # tools/shader_aot.py's date
        os.utime(cache / 'v7_ahead_of_time.dxbc', (unplayed, unplayed))
        names = [item.name for item in bundler.build_plan(self.root) if item.name.endswith('.dxbc')]
        self.assertEqual(names, ['dump/shadercache/v7_played.dxbc'])

    def test_verification_rejects_tampering(self):
        path = self.package()
        with zipfile.ZipFile(path) as archive:
            manifest = archive.read('SSBU-Portable/manifest.json')
        entries = json.loads(manifest)['files']
        entries[0]['sha256'] = '0' * 64
        with self.assertRaisesRegex(RuntimeError, 'integrity check failed'):
            bundler.verify_archive(path, entries, manifest)


if __name__ == '__main__':
    unittest.main(verbosity=2)
