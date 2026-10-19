"""tools/gamefiles.py: what a folder of game files holds, with made-up records and empty files."""
from pathlib import Path
import os
import sys
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import gamefiles  # noqa: E402

APP = 0x01006A800016E000
OTHER = 0x0100000000010000


def record(name, kind, app=APP, type='program', container=None, version=None):
    title = app if kind == 'base' else app + 0x800 if kind == 'update' else app + 0x1001
    return {'path': f'D:\\MyDump\\{name}', 'name': name, 'container': container, 'type': type,
            'title_id': f'{title:016X}', 'app': f'{app:016X}', 'kind': kind, 'version': version,
            'romfs': 'plain', 'content_id': None, 'size': 1, 'display_version': None, 'game': None}


class TitleIdTests(unittest.TestCase):
    def test_kinds(self):
        self.assertEqual(gamefiles.kind_of(APP), 'base')
        self.assertEqual(gamefiles.kind_of(APP + 0x800), 'update')
        self.assertEqual(gamefiles.kind_of(0x01006A800016F001), 'dlc')
        self.assertEqual(gamefiles.kind_of(0x01006A800016F063), 'dlc')

    def test_app(self):
        for title in (APP, APP + 0x800, 0x01006A800016F001, 0x01006A800016F063):
            self.assertEqual(gamefiles.app_of(title), APP, hex(title))
        self.assertEqual(gamefiles.app_of(OTHER + 0x800), OTHER)

    def test_version_string(self):
        self.assertEqual(gamefiles.version_string(917504), 'v14')
        self.assertEqual(gamefiles.version_string(0), 'v0')


class PickProgramTests(unittest.TestCase):
    def test_base_alone(self):
        self.assertEqual(gamefiles.pick_program([record('program.nca', 'base'), record('control.nca', 'base', type='control')])['name'], 'program.nca')

    def test_update_over_base(self):
        got = gamefiles.pick_program([record('base.nca', 'base'), record('update.nca', 'update')])
        self.assertEqual(got['name'], 'update.nca')

    def test_newest_of_several_updates(self):
        got = gamefiles.pick_program([record('u12.nca', 'update', version=12 << 16), record('u13.nca', 'update', version=14 << 16),
                                      record('base.nca', 'base')])
        self.assertEqual(got['name'], 'u13.nca')

    def test_several_updates_without_versions(self):
        with self.assertRaises(gamefiles.Error):
            gamefiles.pick_program([record('a.nca', 'update'), record('b.nca', 'update')])

    def test_the_games_own_among_other_games(self):
        got = gamefiles.pick_program([record('other.nca', 'base', app=OTHER), record('ours.nca', 'base'), record('theirs.nca', 'update', app=OTHER)])
        self.assertEqual(got['name'], 'ours.nca')

    def test_another_game_alone_is_taken(self):
        self.assertEqual(gamefiles.pick_program([record('x.nca', 'base', app=OTHER)])['name'], 'x.nca')

    def test_several_other_games(self):
        with self.assertRaises(gamefiles.Error):
            gamefiles.pick_program([record('x.nca', 'base', app=OTHER), record('y.nca', 'base', app=OTHER + 0x1000000)])

    def test_packaged_program_does_not_count(self):
        with self.assertRaises(gamefiles.Error):
            gamefiles.pick_program([record('update.nca', 'update', container='update.nsp'), record('meta.nca', 'base', type='meta')])
        with self.assertRaises(gamefiles.Error):
            gamefiles.pick_program([])

    def test_base_for_an_update(self):
        contents = [record('base.nca', 'base'), record('update.nca', 'update'), record('other.nca', 'base', app=OTHER)]
        self.assertEqual(gamefiles.base_program(contents, contents[1])['name'], 'base.nca')
        self.assertIsNone(gamefiles.base_program([contents[1], contents[2]], contents[1]))


class SummaryTests(unittest.TestCase):
    def test_lines(self):
        contents = [record('base.nca', 'base'), record('update.nca', 'update', version=14 << 16),
                    record('dlc1.nca', 'dlc', type='data'), record('other.nca', 'base', app=OTHER)]
        contents[1]['display_version'] = '13.0.5'
        contents[2]['title_id'] = f'{APP + 0x1002:016X}'
        contents.append(dict(record('dlc2.nca', 'dlc', type='data'), title_id=f'{APP + 0x1003:016X}'))
        lines = gamefiles.summary({'contents': contents, 'unread': ['bad.nca: cut off']})
        text = '\n'.join(lines)
        self.assertIn("the game (base)", text)
        self.assertIn("update: 13.0.5", text)
        self.assertIn('DLC: 2 add-on contents', text)
        self.assertIn('1 other game here, ignored', text)
        self.assertIn('1 file could not be read', text)
        self.assertEqual(gamefiles.summary({'contents': [], 'unread': []}), ['No game files (.nca or .nsp) found here.'])


class FolderTests(unittest.TestCase):
    def test_list_ncas_files_folders_and_packages(self):
        with tempfile.TemporaryDirectory(prefix='gamefiles-test-') as tmp:
            root = Path(tmp)
            (root / 'a.nca').write_bytes(b'')
            (root / 'B.NCA').write_bytes(b'')
            (root / '0123456789abcdef0123456789abcdef.nca').mkdir()
            (root / '0123456789abcdef0123456789abcdef.nca' / '00').write_bytes(b'')
            (root / 'empty.nca').mkdir()
            (root / 'meta.cnmt.nca').write_bytes(b'')
            (root / 'dlc').mkdir()
            (root / 'dlc' / 'pack.nsp').write_bytes(b'')
            (root / 'dlc' / 'c.nca').write_bytes(b'')
            (root / 'dlc' / 'deeper').mkdir()
            (root / 'dlc' / 'deeper' / 'd.nca').write_bytes(b'')
            (root / 'notes.txt').write_bytes(b'')
            ncas, nsps = gamefiles.list_ncas(str(root))
            names = [n for _p, n in ncas]
            self.assertEqual(sorted(names), ['0123456789abcdef0123456789abcdef.nca', 'B.NCA', 'a.nca', 'c.nca', 'meta.cnmt.nca'])
            self.assertEqual(names[:3], ['0123456789abcdef0123456789abcdef.nca', 'B.NCA', 'a.nca'])   # a folder's own files in name order
            self.assertTrue(ncas[0][0].endswith(os.sep + '00'))
            self.assertEqual([n for _p, n in nsps], ['pack.nsp'])
            ncas0, _ = gamefiles.list_ncas(str(root), depth=0)
            self.assertNotIn('c.nca', [n for _p, n in ncas0])

    def test_describe_nca(self):
        nca = types.SimpleNamespace(content_type=0, title_id=APP + 0x800, size=123, plaintext=False,
                                    section=lambda fs_type: types.SimpleNamespace(enc_type=4))
        rec = gamefiles.describe_nca(nca, r'D:\x\0123456789abcdef0123456789abcdef.nca\00', '0123456789abcdef0123456789abcdef.nca')
        self.assertEqual((rec['type'], rec['kind'], rec['romfs'], rec['content_id']), ('program', 'update', 'patch', '0123456789abcdef0123456789abcdef'))
        plain = types.SimpleNamespace(content_type=0, title_id=APP, size=5, plaintext=True, section=lambda fs_type: types.SimpleNamespace(enc_type=3))
        rec = gamefiles.describe_nca(plain, r'D:\MyDump\program.nca', 'program.nca')
        self.assertEqual((rec['kind'], rec['romfs'], rec['content_id']), ('base', 'plain', None))
        control = types.SimpleNamespace(content_type=2, title_id=APP, size=5, plaintext=False, section=lambda fs_type: None)
        self.assertEqual(gamefiles.describe_nca(control, 'c', 'c.nca')['type'], 'control')


if __name__ == '__main__':
    unittest.main()
