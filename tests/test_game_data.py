"""tools/game_data.py: what Configure-Game-Data.ps1 checks and writes (tests/test_configure.py, on
Windows), on any system, and romfs.cfg's format on each."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import game_data  # noqa: E402

FONTS = game_data.FONT_FILES


class ConfigureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='game-data-test-')
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.install = self.folder / 'runtime with spaces'
        self.install.mkdir()
        self.romfs = self.folder / 'romfs.bin'
        self.romfs.write_bytes(struct.pack('<10Q', 80, 80, 0, 80, 0, 80, 0, 80, 0, 80) + b'fixture')
        self.fonts = self.folder / 'my fonts'
        self.fonts.mkdir()
        for name in FONTS:
            (self.fonts / name).write_bytes(b'font-fixture')
        self.aoc = self.folder / 'aoc.cfg'
        self.aoc.write_text('1\n')

    def configure(self, **kw):
        args = dict(romfs=str(self.romfs), fonts=str(self.fonts), aoc=str(self.aoc), root=str(self.install), windows=False)
        args.update(kw)
        return game_data.configure(**args)

    def test_setup_and_backup(self):
        self.configure()
        config = self.install / 'dump/modules/romfs.cfg'
        self.assertEqual(config.read_bytes(), f'{self.romfs}\n0\n'.encode('utf-8'))
        self.assertEqual((self.install / 'dump/modules/aoc.cfg').read_bytes(), self.aoc.read_bytes())
        for name in FONTS:
            self.assertEqual((self.install / 'dump/fonts' / name).read_bytes(), b'font-fixture')
        self.configure()
        backups = list(config.parent.glob('romfs.cfg.before-setup-*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), config.read_bytes())
        self.assertEqual(list(config.parent.glob('*.tmp')), [])

    def test_fonts_already_in_place(self):
        """build_game.py names dump/fonts itself as the fonts folder: nothing is copied over itself."""
        self.configure()
        self.configure(fonts=str(self.install / 'dump/fonts'), aoc=None)
        self.assertEqual(list((self.install / 'dump/fonts').glob('*.before-setup-*')), [])

    def test_dry_run_writes_nothing(self):
        self.assertIsNone(self.configure(dry_run=True))
        self.assertFalse((self.install / 'dump').exists())

    def test_bad_image_rejected(self):
        self.romfs.write_bytes(bytes(100))
        with self.assertRaises(game_data.Error):
            self.configure()
        self.assertFalse((self.install / 'dump').exists())

    def test_tables_outside_file_rejected(self):
        self.romfs.write_bytes(struct.pack('<10Q', 80, 80, 0, 80, 10**6, 80, 0, 80, 0, 80))
        with self.assertRaisesRegex(game_data.Error, 'outside the file'):
            self.configure()

    def test_offset_outside_file_rejected(self):
        with self.assertRaisesRegex(game_data.Error, 'offset is outside'):
            self.configure(offset=10**6)

    def test_missing_font_rejected(self):
        (self.fonts / FONTS[0]).unlink()
        with self.assertRaisesRegex(game_data.Error, 'Missing required font'):
            self.configure()
        self.assertFalse((self.install / 'dump').exists())

    def test_truncated_font_rejected(self):
        (self.fonts / FONTS[1]).write_bytes(b'tiny')
        with self.assertRaisesRegex(game_data.Error, 'Truncated'):
            self.configure()


class FormatTests(unittest.TestCase):
    def test_by_system(self):
        self.assertEqual(game_data.cfg_format(windows=False), ('utf-8', '\n'))
        self.assertEqual(game_data.cfg_format(windows=True), ('mbcs', '\r\n'))


if __name__ == '__main__':
    unittest.main()
