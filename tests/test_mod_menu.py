"""The start-up menu's Mods page (runtime/mod_menu.c) and the mods folder (runtime/mod_scan.c) without
a window: tests/mod_menu_test.c, built here with the host's C compiler, plays scripts of keys against a
mods folder made for each test and prints the page."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
CC = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
TOOLCHAIN = Path(os.environ.get('SSBU_LINUX_TOOLCHAIN') or ROOT.parent / 'linux-toolchain')
DXVK_INCLUDE = TOOLCHAIN / 'dxvk-headers' / 'include' / 'native'

SKIN = 'fighter/palutena/model/body/c03'


@unittest.skipUnless(sys.platform.startswith('linux') and CC and (DXVK_INCLUDE / 'directx').is_dir(),
                     'Linux, a C compiler and the Linux toolchain\'s DXVK headers')
class ModMenuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='mod-menu-test-')
        cls.exe = Path(cls.temp.name) / 'mod_menu_test'
        flags = ['-O1', '-w', '-D_GNU_SOURCE', '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime/linux/include'),
                 '-I', str(DXVK_INCLUDE / 'windows'), '-I', str(DXVK_INCLUDE / 'directx')]
        r = subprocess.run([CC, *flags, str(ROOT / 'tests/mod_menu_test.c'), str(ROOT / 'runtime/mod_menu.c'),
                            str(ROOT / 'runtime/mod_scan.c'), '-o', str(cls.exe)], capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(f'compiling the test failed:\n{r.stdout}{r.stderr}')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        d = tempfile.TemporaryDirectory(prefix='mod-menu-run-')
        self.addCleanup(d.cleanup)
        self.mods = Path(d.name) / 'mods'

    def mod(self, folder, files, info=None):
        for f in files:
            p = self.mods / folder / f
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(b'x' * 16)
        if info:
            (self.mods / folder / 'info.toml').write_text(info)

    def skin(self, folder, fighter='palutena', color=3, **kw):
        self.mod(folder, [f'fighter/{fighter}/model/body/c{color:02d}/model.numdlb',
                          f'ui/replace/chara/chara_3/chara_3_{fighter}_{color:02d}.bntx'], **kw)

    def run_menu(self, script='', **env):
        base = {k: v for k, v in os.environ.items() if not k.startswith('SSBU_')}
        r = subprocess.run([str(self.exe), str(self.mods), script], capture_output=True, text=True, env={**base, **env}, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        out = {'mods': [], 'rows': []}
        for line in r.stdout.splitlines():
            k, _, v = line.partition('=')
            if k == 'mod':
                out['mods'].append(v)
            elif k.startswith('row'):
                out['rows'].append(v)
            else:
                out[k] = v
        return out

    def test_empty_folder_is_made(self):
        out = self.run_menu()
        self.assertTrue(self.mods.is_dir())
        self.assertEqual(out['sel'], '(button)')
        self.assertIn('No mods yet', out['rows'][0])
        self.assertEqual(self.run_menu('a')['folder'], '1')

    def test_list_info_and_classification(self):
        self.skin('Frieren over Palutena', info='display_name = "Ancient Magus Frieren"\nauthor = "Sombra"\nversion = "1.0.0"\n'
                                              'description = """\nFrieren ends\nher journey\n"""\n')
        self.mod('Params', ['fighter/mario/param/vl.prc'])
        self.mod('.Old skin', [f'{SKIN}/a.nutexb'])
        out = self.run_menu()
        self.assertEqual(out['mods'], ['Frieren over Palutena:E:palutena:c03:0:0:Ancient Magus Frieren',
                                       'Old skin:D:palutena:c03:0:0:Old skin',
                                       'Params:E:mario::0:1:Params'])
        self.assertIn('Ancient Magus Frieren | Palutena - color 4 | Enabled', out['rows'][0])
        detail = out['rows'][3]
        self.assertIn('Frieren ends her journey', detail)
        self.assertIn('Author: Sombra     Version: 1.0.0', detail)
        self.assertIn('Palutena - color 4 - model, portraits', detail)
        self.assertIn('Visual only', detail)
        self.assertIn('Palutena|...M....|0', detail)
        params = self.run_menu('dd')['rows'][3]
        self.assertIn('it can desync online play', params)

    def test_enable_disable_and_name_taken(self):
        self.skin('Skin')
        out = self.run_menu('a')
        self.assertTrue((self.mods / '.Skin').is_dir())
        self.assertEqual(out['mods'][0][:7], 'Skin:D:')
        self.assertIn('Skin disabled', out['note'])
        self.assertTrue(self.run_menu('a')['mods'][0].startswith('Skin:E:'))
        self.skin('.Skin')   # both: disabling would overwrite
        out = self.run_menu('ddd')
        sel = out['sel']
        out = self.run_menu('a' if sel == 'Skin' else 'da')
        self.assertTrue(out['note'].startswith('!Cannot rename'), out['note'])
        self.assertTrue((self.mods / 'Skin').is_dir() and (self.mods / '.Skin').is_dir())

    def test_delete_asks_first(self):
        self.skin('Doomed')
        out = self.run_menu('x')
        self.assertEqual(out['dialog'], '1 0')
        self.assertIn('Delete "Doomed" and all its files?', out['question'])
        self.run_menu('xa')   # Cancel is chosen at first
        self.assertTrue((self.mods / 'Doomed').is_dir())
        self.run_menu('xrb')   # Esc never deletes
        self.assertTrue((self.mods / 'Doomed').is_dir())
        out = self.run_menu('xra')
        self.assertFalse((self.mods / 'Doomed').exists())
        self.assertEqual(out['mods'], [])
        self.assertIn('Doomed deleted', out['note'])

    def test_delete_a_link_keeps_what_it_points_to(self):
        elsewhere = self.mods.parent / 'elsewhere'
        (elsewhere / SKIN).mkdir(parents=True)
        (elsewhere / SKIN / 'model.numdlb').write_bytes(b'x')
        self.mods.mkdir()
        (self.mods / 'Linked').symlink_to(elsewhere)
        self.run_menu('xra')
        self.assertFalse((self.mods / 'Linked').exists())
        self.assertTrue((elsewhere / SKIN / 'model.numdlb').exists())

    def test_move_and_swap(self):
        self.skin('A Frieren')
        self.skin('B Other', color=6)
        out = self.run_menu('zrrra')   # c03 -> c06, where B is: B to c03
        self.assertEqual(out['mods'], ['A Frieren:E:palutena:c06:0:0:A Frieren', 'B Other:E:palutena:c03:0:0:B Other'])
        self.assertIn('Moved to color 7, B Other to its place', out['note'])
        slots = (self.mods / 'windowsmash.toml').read_text()
        self.assertIn('[mod."A Frieren"]\npalutena.c03 = "c06"', slots)
        self.assertIn('[mod."B Other"]\npalutena.c06 = "c03"', slots)
        self.assertEqual(self.run_menu()['mods'][0], 'A Frieren:E:palutena:c06:0:0:A Frieren')   # read back

    def test_move_cancel_and_limits(self):
        self.skin('Skin', color=6)
        out = self.run_menu('zrrrr')
        self.assertEqual(out['moving'], '1 1')   # only up to c07
        self.assertIn('Palutena|.......M|1', out['rows'][1])
        out = self.run_menu('zrrb')
        self.assertEqual((out['moving'], out['mods'][0].split(':')[3]), ('0 0', 'c06'))
        self.assertFalse((self.mods / 'windowsmash.toml').exists())
        self.mod('Two fighters', ['fighter/mario/model/body/c00/a', 'fighter/luigi/model/body/c00/a'])
        out = self.run_menu('dz')
        self.assertEqual(out['moving'], '0 0')
        self.assertIn('more than one fighter', out['note'])

    def test_kirby_copy_voice_and_camera_belong_to_the_fighter(self):
        # (aMiku: Lucina c03 with Kirby's copy of her, her voice, sounds, camera and effects)
        self.mod('Miku', ['fighter/lucina/model/body/c03/model.numdlb', 'fighter/kirby/model/copy_lucina_mask/c03/model.numdlb',
                          'fighter/kirby/model/copy_lucina_sword/c03/model.numdlb', 'sound/bank/fighter_voice/vc_lucina_c03.nus3audio',
                          'sound/bank/fighter/se_lucina_c03.nus3audio', 'camera/fighter/lucina/c03/j02win1.nuanmb',
                          'effect/fighter/lucina/ef_lucina_c03.eff', 'effect/fighter/lucina/trail_c03/tex_lucina_sword1.nutexb'])
        self.assertEqual(self.run_menu()['mods'][0].split(':')[2:4], ['lucina', 'c03'])
        out = self.run_menu('zrrra')   # moved by three: c06
        self.assertEqual(out['mods'][0].split(':')[3], 'c06')

    def test_color_from_the_folder_name(self):
        self.skin('palutena-frieren-c06')
        self.assertEqual(self.run_menu()['mods'][0].split(':')[3], 'c06')
        self.skin('bad-c09')
        self.assertEqual(self.run_menu('d')['mods'][0].split(':')[3], 'c03')
        # windowsmash.toml wins over the name
        (self.mods / 'windowsmash.toml').write_text('[mod."palutena-frieren-c06"]\npalutena.c03 = "c01"\n')
        self.assertEqual(self.run_menu()['mods'][1].split(':')[3], 'c01')

    def test_conflicts_after_moving(self):
        self.skin('A')
        self.skin('B', color=5)
        self.assertEqual([m.split(':')[4] for m in self.run_menu()['mods']], ['0', '0'])
        (self.mods / 'windowsmash.toml').write_text('[mod."B"]\npalutena.c05 = "c03"\n')
        out = self.run_menu()
        self.assertEqual([m.split(':')[4] for m in out['mods']], ['2', '2'])
        self.assertIn('2 files also in B', out['rows'][2])

    def test_mods_off(self):
        self.skin('Skin')
        out = self.run_menu(SSBU_MODS='0')
        self.assertEqual(out['mods'], [])
        self.assertIn('Mods are off', out['rows'][0])

    def test_navigation_wraps_through_the_button(self):
        self.skin('A')
        self.skin('B')
        self.assertEqual(self.run_menu('d')['sel'], 'B')
        self.assertEqual(self.run_menu('dd')['sel'], '(button)')
        self.assertEqual(self.run_menu('ddd')['sel'], 'A')
        self.assertEqual(self.run_menu('u')['sel'], '(button)')
        out = self.run_menu('b')
        self.assertEqual(out['backs'], '1')
        self.assertEqual(self.run_menu('y')['folder'], '1')


if __name__ == '__main__':
    unittest.main()
