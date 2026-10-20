from pathlib import Path
import os
import runpy
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
BUILD = runpy.run_path(str(ROOT / 'tools/build_game.py'), run_name='build_game_reference')
sys.path.insert(0, str(ROOT / 'tests'))
import test_arc_table as arc_table  # noqa: E402  (made-up data.arc's)
pick_program, jobs_for_ram, romfs_cfg_ok = BUILD['pick_program'], BUILD['jobs_for_ram'], BUILD['romfs_cfg_ok']
Failed = BUILD['Failed']
PROGRAM, META, CONTROL = 0, 1, 2
BASE, UPDATE = 0x01006A800016E000, 0x01006A800016E800


class PickProgramTests(unittest.TestCase):
    def test_the_one_program(self):
        ncas = [('meta.cnmt.nca', META, BASE), ('program.nca', PROGRAM, BASE), ('control.nca', CONTROL, BASE)]
        self.assertEqual(pick_program(ncas), 'program.nca')

    def test_of_two_the_update(self):
        ncas = [('base.nca', PROGRAM, BASE), ('update.nca', PROGRAM, UPDATE), ('control.nca', CONTROL, UPDATE)]
        self.assertEqual(pick_program(ncas), 'update.nca')

    def test_no_program(self):
        with self.assertRaises(Failed):
            pick_program([('meta.cnmt.nca', META, BASE), ('control.nca', CONTROL, BASE)])
        with self.assertRaises(Failed):
            pick_program([])

    def test_ambiguous(self):
        with self.assertRaises(Failed):
            pick_program([('a.nca', PROGRAM, BASE), ('b.nca', PROGRAM, BASE)])
        with self.assertRaises(Failed):
            pick_program([('a.nca', PROGRAM, UPDATE), ('b.nca', PROGRAM, UPDATE)])


class GameSourceTests(unittest.TestCase):
    def test_same_game(self):
        same_game, describe = BUILD['same_game'], BUILD['describe_identity']
        identity = BUILD['game_identity']
        update = {'app': '01006A800016E000', 'kind': 'update', 'version': 2031616}
        base = {'app': '01006A800016E000', 'kind': 'base', 'version': 0}
        self.assertEqual(identity(update), '01006A800016E000 update 2031616')
        self.assertEqual(identity(dict(base, version=None)), '01006A800016E000 base -')
        self.assertTrue(same_game(identity(update), identity(update)))
        self.assertFalse(same_game(identity(base), identity(update)))              # 11.0.0 folder, 13.0.5 files
        self.assertFalse(same_game(identity(update), identity(dict(update, version=1966080))))
        self.assertTrue(same_game(identity(base), identity(dict(base, version=None))))   # unknown: not held against it
        self.assertEqual(describe(identity(update)), "the game's update (title version v31)")
        self.assertEqual(describe(identity(update), '13.0.5'), "13.0.5 (the game's update)")
        self.assertEqual(describe('01006A800016E000 base -'), 'the game')


class BuildStepsTests(unittest.TestCase):
    """build() from the folders to the fighters, with what reads game files stubbed: which tools it
    runs, and with what."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='build-steps-test-')
        self.addCleanup(self.temp.cleanup)
        top = Path(self.temp.name)
        self.game, keys, firmware = top / 'game', top / 'keys', top / 'firmware'
        for d in (self.game, keys, firmware):
            d.mkdir()
        dump = top / 'port' / 'dump'
        exefs = dump / 'exefs'
        exefs.mkdir(parents=True)
        (dump / 'modules').mkdir()
        for n in ('main', 'main.npdm', 'rtld', 'sdk', 'subsdk0'):   # (the program is copied out already)
            (exefs / n).write_bytes(b'')
        self.program = {'path': str(self.game / 'update.nsp'), 'name': 'prog.nca', 'container': 'update.nsp',
                        'app': '01006A800016E000', 'kind': 'update', 'version': 2031616, 'title_id': '01006A800016E000'}
        self.contents = [self.program]
        self.calls = []
        self.logged = []
        # (the module's own globals, which build() reads: run_path returns a copy of them)
        self.ns = runpy.run_path(str(ROOT / 'tools/build_game.py'), run_name='build_game_steps')['build'].__globals__
        self.check_game_data = self.ns['check_game_data']
        self.cfg = dump / 'modules' / 'romfs.cfg'
        folders = {'game': str(self.game), 'keys': str(keys), 'firmware': str(firmware)}
        self.ns.update(
            ROOT=str(top / 'port'), DUMP=str(dump), EXEFS=str(exefs), FONTS=str(dump / 'fonts'), MODULES=str(dump / 'modules'),
            SAVED=str(dump / 'build_folders.txt'), SOURCE=str(dump / 'game_source.txt'),
            log=lambda msg='': self.logged.append(msg), check_tools=lambda: None, install_packages=lambda: None,
            ask_folder=lambda key, given, saved: folders[key],
            find_program=lambda game, keys: (self.program['path'], BASE, self.program, {'contents': self.contents}),
            romfs_cfg_ok=lambda cfg: True, missing_fonts=lambda folder: [], check_same_game=lambda rec: None,
            check_game_data=lambda cfg, game: None,
            run=lambda args, failure: self.calls.append([str(a) for a in args]))
        self.args = types.SimpleNamespace(game=str(self.game), keys=str(keys), firmware=str(firmware), jobs=None,
                                          prepare_only=True)

    def aoc_call(self):
        calls = [c for c in self.calls if 'aoc-cfg' in c]
        return calls[0] if calls else None

    def test_dlc_next_to_the_game(self):
        # an emulator's folder, or loose DLC .nsp files: no dlc folder (this raised KeyError: 1)
        self.contents.append({'kind': 'dlc', 'app': '01006A800016E000'})
        self.ns['build'](self.args)
        call = self.aoc_call()
        self.assertEqual(call[call.index('aoc-cfg') + 1], str(self.game))
        self.assertEqual(call[call.index('--app') + 1], '01006A800016E000')
        self.assertTrue(any('arc.py' in ' '.join(c) for c in self.calls))   # and on to the fighters

    def test_dlc_folder(self):
        (self.game / 'dlc').mkdir()
        self.ns['build'](self.args)
        call = self.aoc_call()
        self.assertEqual(call[call.index('aoc-cfg') + 1], str(self.game / 'dlc'))

    def test_no_dlc(self):
        self.contents.append({'kind': 'dlc', 'app': '0100000000010000'})   # another game's
        self.ns['build'](self.args)
        self.assertIsNone(self.aoc_call())
        self.assertTrue(any('arc.py' in ' '.join(c) for c in self.calls))

    def game_data(self, parts):
        """romfs.cfg (step 6's, done) reading a RomFS that holds this data.arc (tests/test_arc_table.py)."""
        image = Path(self.temp.name) / 'romfs.bin'
        image.write_bytes(arc_table.romfs(arc_table.flat(parts)))
        self.cfg.write_text(f'{image}\n0\n')
        self.ns['check_game_data'] = self.check_game_data

    def test_an_early_release_stops_at_step_6(self):
        self.game_data(arc_table.EARLY[('uncompressed', 'v1')])
        with self.assertRaises(self.ns['Failed']) as e:
            self.ns['build'](self.args)
        text = str(e.exception)
        self.assertIn(f'The game in {self.game} is an early release', text)
        self.assertIn('uncompressed file table, v1 layout', text)
        self.assertIn(f"Put the game's update (an .nsp file; the newest you have) in {self.game}", text)
        self.assertIn('extract a fresh WindowSmash folder and run Install WindowSmash.bat there', text)
        self.assertNotIn('romfs.bin', text)
        steps = [m.strip() for m in self.logged if m.lstrip().startswith('[')]
        self.assertTrue(steps[-1].startswith('[6/9]'), steps[-1])
        self.assertEqual(self.calls, [])   # (no DLC, no fighters)

    def test_a_newer_release_goes_on(self):
        self.game_data(arc_table.pieces(arc_table.zstd_header()))
        self.ns['write_version'] = lambda: None   # (that header is all there is to read)
        self.ns['build'](self.args)
        self.assertTrue(any('arc.py' in ' '.join(c) for c in self.calls))

    def test_unreadable_game_data(self):
        self.game_data({0: bytes(0x38)})   # (no data.arc as the game's begins)
        with self.assertRaises(self.ns['Failed']) as e:
            self.ns['build'](self.args)
        self.assertIn('damaged or incomplete', str(e.exception))

    def test_told_by_the_scan_before_decrypting(self):
        self.ns['romfs_cfg_ok'] = lambda cfg: False   # (step 6 to do)
        self.program.update(kind='base', romfs='encrypted', data_arc=['zstd', 'v1'], display_version='1.0.0')
        with self.assertRaises(self.ns['Failed']) as e:
            self.ns['build'](self.args)
        self.assertIn('zstd file table, v1 layout', str(e.exception))
        self.assertFalse(any('decrypt' in c for c in self.calls))


class MarkerTests(unittest.TestCase):
    def test_python_version_markers(self):
        marker_applies = BUILD['marker_applies']
        here = sys.version_info[:2]
        self.assertEqual(marker_applies('python_version < "3.14"'), here < (3, 14))
        self.assertEqual(marker_applies(' python_version >= "3.14" '), here >= (3, 14))
        self.assertTrue(marker_applies('python_version >= "3.10"'))
        self.assertFalse(marker_applies('python_version < "3.0"'))
        self.assertFalse(marker_applies('python_version >= "3.10" and python_version < "3.1"'))
        self.assertTrue(marker_applies('platform_system == "Linux"'))   # (unknown clauses are taken as holding)


class JobsTests(unittest.TestCase):
    def test_by_installed_ram(self):
        for gb, jobs in ((4, 4), (8, 4), (12, 4), (15.9, 4), (16, 8), (24, 8), (32, 16), (64, 16)):
            self.assertEqual(jobs_for_ram(gb), jobs, gb)


class RomfsCfgTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='build-game-test-')
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.image = self.folder / 'romfs.bin'
        self.image.write_bytes(b'image')
        self.cfg = self.folder / 'romfs.cfg'

    def test_plain(self):
        self.assertFalse(romfs_cfg_ok(self.cfg))
        self.cfg.write_text(f'{self.image}\n0\n')
        self.assertTrue(romfs_cfg_ok(self.cfg))
        self.image.unlink()
        self.assertFalse(romfs_cfg_ok(self.cfg))

    def test_layered(self):
        layers = self.folder / 'romfs.layers'
        layers.write_bytes(b'layers')
        self.cfg.write_text(f'layered\n{layers}\n{self.image}\n{self.folder / "patch.bin"}\n')
        self.assertFalse(romfs_cfg_ok(self.cfg))
        (self.folder / 'patch.bin').write_bytes(b'patch')
        self.assertTrue(romfs_cfg_ok(self.cfg))


class VersionFileTests(unittest.TestCase):
    """version.txt, by which the launchers name the game version (tools/pick_version.bat): without
    one, the port's own folder showed as "Game version .  - build\\, save\\"."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='version-test-')
        self.addCleanup(self.temp.cleanup)
        self.top = Path(self.temp.name)
        self.logged = []
        self.ns = runpy.run_path(str(ROOT / 'tools/build_game.py'), run_name='build_game_version')['build'].__globals__
        self.ns.update(ROOT=str(self.top), MODULES=str(self.top / 'dump' / 'modules'), EXEFS=str(self.top / 'dump' / 'exefs'),
                       log=self.logged.append)
        self.arc = types.ModuleType('arc')
        self.arc.Arc = lambda cfg: types.SimpleNamespace(version=0x0D0005)   # (data.arc 13.0.5)
        nso = types.ModuleType('nso')
        nso.Module = lambda path: types.SimpleNamespace(build_id='ABCD')
        patcher = mock.patch.dict(sys.modules, {'arc': self.arc, 'nso': nso})
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_written_once(self):
        self.ns['write_version']()
        text = (self.top / 'version.txt').read_text()
        self.assertEqual(text.splitlines()[0], '13.0.5')
        self.assertTrue(text.endswith('main ABCD\ndata.arc 13.0.5\n'))
        (self.top / 'version.txt').write_text('13.0.5 (mine)\n')
        self.ns['write_version']()   # (one folder, one version: a version.txt there is kept)
        self.assertEqual((self.top / 'version.txt').read_text(), '13.0.5 (mine)\n')

    def test_unreadable_does_not_stop_the_build(self):
        def broken(cfg):
            raise OSError('no game data')
        self.arc.Arc = broken
        self.ns['write_version']()
        self.assertFalse((self.top / 'version.txt').exists())
        self.assertIn('could not be read', self.logged[-1])

    @unittest.skipUnless(os.name == 'nt', 'a batch file')
    def test_the_launchers_name_it(self):
        (self.top / 'tools').mkdir()
        shutil.copy(ROOT / 'tools' / 'pick_version.bat', self.top / 'tools' / 'pick_version.bat')
        (self.top / 'build').mkdir()
        (self.top / 'build' / 'ssbu.exe').write_bytes(b'')

        def picked():
            r = subprocess.run(['cmd', '/d', '/c', r'call tools\pick_version.bat'], cwd=self.top, capture_output=True, text=True)
            return r.stdout.strip()
        self.assertEqual(picked(), 'Game version .  - build\\, save\\')   # (what users saw)
        self.ns['write_version']()
        self.assertEqual(picked(), 'Game version 13.0.5  - build\\, save\\')


if __name__ == '__main__':
    unittest.main()
