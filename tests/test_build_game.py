from pathlib import Path
import runpy
import tempfile
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
