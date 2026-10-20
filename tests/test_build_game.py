from pathlib import Path
import runpy
import sys
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parent.parent
BUILD = runpy.run_path(str(ROOT / 'tools/build_game.py'), run_name='build_game_reference')
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
        # (the module's own globals, which build() reads: run_path returns a copy of them)
        self.ns = runpy.run_path(str(ROOT / 'tools/build_game.py'), run_name='build_game_steps')['build'].__globals__
        folders = {'game': str(self.game), 'keys': str(keys), 'firmware': str(firmware)}
        self.ns.update(
            DUMP=str(dump), EXEFS=str(exefs), FONTS=str(dump / 'fonts'), MODULES=str(dump / 'modules'),
            SAVED=str(dump / 'build_folders.txt'), SOURCE=str(dump / 'game_source.txt'),
            log=lambda msg='': None, check_tools=lambda target="windows": None, install_packages=lambda: None,
            ask_folder=lambda key, given, saved: folders[key],
            find_program=lambda game, keys: (self.program['path'], BASE, self.program, {'contents': self.contents}),
            romfs_cfg_ok=lambda cfg: True, missing_fonts=lambda folder: [], check_same_game=lambda rec: None,
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


if __name__ == '__main__':
    unittest.main()
