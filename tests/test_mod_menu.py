"""The start-up menu's Mods page (runtime/mod_menu.c) and the mods folder (runtime/mod_scan.c) without
a window: tests/mod_menu_test.c, built here with the host's C compiler, plays scripts of keys against a
mods folder made for each test and prints the page."""
from pathlib import Path
import os
import re
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
        r = subprocess.run([CC, *flags, '-pthread', str(ROOT / 'tests/mod_menu_test.c'), str(ROOT / 'runtime/mod_menu.c'),
                            str(ROOT / 'runtime/mod_scan.c'), str(ROOT / 'runtime/mod_roster.c'), str(ROOT / 'runtime/mod_recovery.c'),
                            *[str(ROOT / 'runtime' / f) for f in ('mod_cache.c', 'mod_fetch.c', 'mod_sync.c', 'mod_zip.c', 'mod_manifest.c', 'mod_sha256.c')],
                            '-lm', '-o', str(cls.exe)], capture_output=True, text=True)
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

    def run_menu(self, script='', grid=False, **env):
        """The page after script: the list of every mod (SSBU_MENU_MODS=list), or with grid the grid of fighters"""
        base = {k: v for k, v in os.environ.items() if not k.startswith('SSBU_')}
        if not grid:
            base['SSBU_MENU_MODS'] = 'list'
        r = subprocess.run([str(self.exe), str(self.mods), script], capture_output=True, text=True, env={**base, **env}, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        out = {'mods': [], 'rows': [], 'sums': {}}
        for line in r.stdout.splitlines():
            k, _, v = line.partition('=')
            if k == 'mod':
                out['mods'].append(v)
            elif k == 'sum':
                name, on, off, mark, why = v.split(':', 4)
                out['sums'][name] = (int(on), int(off), mark, why)
            elif k.startswith('row'):
                out['rows'].append(v)
            else:
                out[k] = v
        return out

    def grid(self, script=''):
        return self.run_menu(script, grid=True)

    def cursor(self, out):
        x, y, shown = out['cursor'].split(',')
        return float(x), float(y), int(shown)

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

    def test_a_skin_with_common_files_moves(self):
        # (Splatsune Inku: Inkling c06 and fighter/common/param/effect.prc - common is no fighter's)
        self.mod('Splatsune', ['fighter/inkling/model/body/c06/model.numdlb', 'fighter/common/param/effect.prc',
                               'ui/replace/chara/chara_0/chara_0_inkling_06.bntx'])
        out = self.run_menu('zlla')   # moved by two down: c04
        self.assertEqual(out['mods'][0].split(':')[2:4], ['inkling', 'c04'])
        self.assertIn('inkling.c06 = "c04"', (self.mods / 'windowsmash.toml').read_text())

    def test_a_move_keeps_the_body(self):
        # Inkling's even colors are the girl, the odd ones the boy (their skeletons and animations): a
        # girl's skin moved to c05 hung the fight's loading. Left from c06 passes over c05, to c04.
        self.mod('Splatsune', ['fighter/inkling/model/body/c06/model.numdlb'])
        out = self.run_menu('zla', TEST_BODY_PARITY='1')
        self.assertEqual(out['mods'][0].split(':')[2:4], ['inkling', 'c04'])
        out = self.run_menu('zrrrr', TEST_BODY_PARITY='1')   # (from c04: c06, then nothing further of hers)
        self.assertEqual(out['moving'], '1 2')

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


    def test_share_in_rooms(self):
        self.skin('palutena-frieren-c06')   # (moved by its name: share = false alone must not undo that)
        self.mod('Params', ['fighter/mario/param/vl.prc'])
        out = self.run_menu('h')   # (the skin is first, then Params)
        self.assertEqual(out['mods'][0], 'palutena-frieren-c06:E:palutena:c06:0:0:palutena-frieren-c06:noshare')
        self.assertIn('not shared in the rooms you host', out['note'])
        self.assertIn('LT=Share', out['rows'][-2])
        self.assertIn('who joins your room does not get it', out['rows'][-2])
        self.assertIn('share = false', (self.mods / 'windowsmash.toml').read_text())
        out = self.run_menu('')   # (read again: kept, and still moved)
        self.assertEqual(out['mods'][0], 'palutena-frieren-c06:E:palutena:c06:0:0:palutena-frieren-c06:noshare')
        out = self.run_menu('dh')   # (Params: a gameplay mod, its name still told)
        self.assertTrue(out['mods'][1].endswith(':noshare'))
        self.assertIn('is told its name, not given its files', out['rows'][-2])
        out = self.run_menu('h')   # (the skin back on)
        self.assertFalse(out['mods'][0].endswith(':noshare'))
        self.assertIn("LT=Don't share", out['rows'][-2])

    def test_room_hosts_listed_and_deleted(self):
        self.skin('Frieren')
        host = self.mods.parent / 'mods_join' / '.hosts' / '00000000000000a1'
        host.mkdir(parents=True)
        import time as _t
        (host / 'host.toml').write_text(f'host_id = "00000000000000a1"\nname = "Alexis"\nlast = {int(_t.time()) - 3 * 86400}\n'
                                        '\n[[mod]]\nid = "1"\nname = "Pikachu Ice"\nfighter = "pikachu"\nactive = true\n'
                                        f'file = "{"ab" * 32} 10 fighter/pikachu/model/body/c04/model.numdlb"\n')
        out = self.run_menu('d')   # (down from the mod: the host's card)
        self.assertEqual(len(out['mods']), 1)   # (mods/.netplay is not a mod)
        self.assertIn("Alexis's room mods", out['rows'][1])
        self.assertIn('last joined 3 days ago', out['rows'][1])
        self.assertIn('X=Delete', out['rows'][-2])
        out = self.run_menu('dx')
        self.assertIn("Delete the mods kept from Alexis's room?", out['question'])
        out = self.run_menu('dxb')   # (Cancel: kept)
        self.assertTrue(host.exists())
        out = self.run_menu('dxra')   # (Delete)
        self.assertFalse(host.exists())
        self.assertIn("Alexis's room mods deleted", out['note'])


    # ---- the grid of fighters (mod-roster-grid) ----

    def test_roster_holds_every_fighter_once(self):
        names = re.findall(r'\{"(\w+)", "', (ROOT / 'runtime/mod_scan.c').read_text().split('FIGHTERS[][2] = {')[1].split('};')[0])
        self.assertGreater(len(names), 90)
        r = subprocess.run([str(self.exe), '--roster'], capture_output=True, text=True, timeout=30)
        tiles = [line.partition('=')[2].split(':') for line in r.stdout.splitlines()]
        held = [n for _, internal, _ in tiles for n in internal.split(',') if n]
        for n in names:
            self.assertEqual(held.count(n), 1, n)
        self.assertEqual([t[0] for t in tiles[:5]], ['Mario', 'Donkey Kong', 'Link', 'Samus', 'Dark Samus'])
        self.assertEqual(tiles[-1][0], 'Other')
        by = {t[0]: t[1].split(',') for t in tiles}
        self.assertEqual(by['Pokemon Trainer'], ['ptrainer', 'pzenigame', 'pfushigisou', 'plizardon'])
        self.assertEqual(by['Ice Climbers'], ['ice_climber', 'popo', 'nana'])
        self.assertEqual(by['Pyra & Mythra'], ['element', 'eflame', 'elight'])
        self.assertNotIn('Charizard', by)
        self.assertEqual([t[0] for t in tiles[-4:-1]], ['Mii Brawler', 'Mii Swordfighter', 'Mii Gunner'])

    def test_grid_sums_and_marks(self):
        self.skin('Fox A', fighter='fox', color=3)
        self.skin('Fox B', fighter='fox', color=3)   # (both in color 4: a problem of colors)
        self.skin('.Fox C', fighter='fox', color=5)
        self.mod('Mario params', ['fighter/mario/param/vl.prc', 'fighter/mario/model/body/c00/model.numdlb'])
        self.mod('.Link new colors', ['fighter/link/model/body/c09/model.numdlb'])
        self.skin('Zard', fighter='plizardon', color=1)
        self.skin('Squirtle', fighter='pzenigame', color=2)
        self.mod('Mario and Luigi', ['fighter/mario/model/body/c00/a', 'fighter/luigi/model/body/c00/a'])
        self.mod('Inkling and common', ['fighter/inkling/model/body/c06/a', 'fighter/common/param/x.prc'])   # (common: no one's)
        sums = self.grid()['sums']
        self.assertEqual(sums['Inkling'][:2], (1, 0))
        self.assertEqual(sums['Fox'][:3], (2, 1, 'x'))
        self.assertIn('Fox A and Fox B are both in color 4', sums['Fox'][3])
        self.assertEqual(sums['Mario'][:3], (1, 0, '!'))
        self.assertIn('changes more than looks', sums['Mario'][3])
        self.assertEqual(sums['Link'][:3], (0, 1, '-'))   # (its c08+ mod disabled: no mark)
        self.assertEqual(sums['Pokemon Trainer'][:3], (2, 0, '-'))
        self.assertEqual(sums['Other'][:3], (1, 0, '-'))
        self.assertNotIn('Luigi', sums)
        row = self.grid()['rows'][0]
        self.assertIn('Ax2,fox,Fox', row)
        self.assertIn('A!1,mario,Mario', row)
        self.assertIn('D 0,link,Link', row)
        self.assertIn('N 0,donkey,Donkey Kong', row)
        self.assertIn('A 2,ptrainer,Pokemon Trainer', row)
        self.assertIn('N 0,eflame_first,Pyra & Mythra', row)   # (the select screen's picture of both)
        self.assertTrue(row.endswith('A 1,,Other'))

    def test_grid_opens_and_goes_back(self):
        self.skin('Frieren')
        out = self.grid()
        self.assertEqual((out['level'], out['tile'], out['sel']), ('0', '0:Mario', '(grid)'))
        self.assertEqual(self.cursor(out), (0.5, 0.5, 1))
        self.assertEqual(len(out['rows']), 2)   # (the grid, the button)
        out = self.grid('Tpalutena;')
        self.assertEqual((out['level'], out['sel']), ('1', 'Frieren'))
        self.assertTrue(out['rows'][0].startswith(': \\24Palutena|...M....|0|palutena'), out['rows'][0])
        self.assertIn('Frieren | Palutena - color 4 | Enabled', out['rows'][1])
        self.assertNotIn('Palutena|...M....', out['rows'][2])   # (the detail: no colors of its own)
        out = self.grid('Tpalutena;a')   # (A on the page: the mod disabled, as before)
        self.assertTrue((self.mods / '.Frieren').is_dir())
        self.assertEqual(out['sums']['Palutena'][:2], (0, 1))
        out = self.grid('Tpalutena;b')
        self.assertEqual((out['level'], out['tile']), ('0', '53:Palutena'))
        self.assertEqual(self.cursor(out), (53 % 12 + 0.5, 53 // 12 + 0.5, 1))
        out = self.grid('C12;rrdda')   # (D-pad: Link, down twice; A opens it)
        self.assertEqual((out['level'], out['tile']), ('1', '26:Mewtwo'))
        self.assertEqual(out['sel'], '(button)')   # (no mods: its page empty)
        self.assertIn('No Mewtwo mods yet', out['rows'][1])

    def test_grid_dpad_to_the_button_and_back(self):
        out = self.grid('C12;' + 'd' * 7)
        self.assertEqual(out['tile'], '84:Mii Swordfighter')   # (86 tiles + Other: the last row has 3)
        out = self.grid('C12;' + 'd' * 8)
        self.assertEqual(out['sel'], '(button)')
        out = self.grid('C12;' + 'd' * 8 + 'u')
        self.assertEqual((out['sel'], out['tile']), ('(grid)', '84:Mii Swordfighter'))
        self.assertEqual(self.grid('C12;' + 'd' * 8 + 'a')['folder'], '1')
        out = self.grid('C12;' + 'd' * 6 + 'r' * 11 + 'd')
        self.assertEqual(out['tile'], '86:Other')

    def test_cursor_with_the_stick(self):
        out = self.grid('C12;S1,0,30;')
        x, y, shown = self.cursor(out)
        self.assertGreater(x, 3)   # (half a second all the way: several tiles)
        self.assertEqual(out['tile'], f'{int(x)}:' + out['tile'].split(':')[1])
        self.assertEqual(self.grid('C12;S1,0,30;')['cursor'], out['cursor'])   # (the same every run)
        slow = self.cursor(self.grid('C12;S0.5,0,30;'))[0]
        self.assertLess(slow - 0.5, (x - 0.5) / 3)   # (half the tilt: a quarter of the speed)
        out = self.grid('C12;S1,0,600;')   # (the edge: it stays there)
        self.assertEqual(out['tile'], '11:Captain Falcon')
        self.assertAlmostEqual(self.cursor(out)[0], 11.999, places=3)
        out = self.grid('C12;S0,1,600;')   # (down: the last row has 3 tiles, column 0 is one of them)
        self.assertEqual(out['tile'], '84:Mii Swordfighter')
        out = self.grid('C12;S1,1,600;')   # (down-right: the row above the last one's empty end)
        self.assertEqual(out['tile'], '83:Mii Brawler')

    def test_cursor_ignores_drift(self):
        out = self.grid('C12;W2500,-1500,0,120;')   # (inside the least dead zone, controllers.ini at 0)
        self.assertEqual(self.cursor(out), (0.5, 0.5, 1))
        out = self.grid('C12;W6000,0,20,120;')   # (inside a 20% one)
        self.assertEqual(self.cursor(out), (0.5, 0.5, 1))
        self.assertGreater(self.cursor(self.grid('C12;W32767,0,20,30;'))[0], 3)

    def test_dpad_from_between_tiles_and_the_mouse(self):
        out = self.grid('C12;S1,0,9;')   # (between Mario and Donkey Kong, or just past)
        x = self.cursor(out)[0]
        t = int(x)
        out = self.grid('C12;S1,0,9;r')
        self.assertEqual(self.cursor(out), (t + 1.5, 0.5, 1))
        out = self.grid('C12;H30;')
        self.assertEqual((out['tile'], self.cursor(out)[2]), ('30:Meta Knight', 0))
        self.assertIn('Meta Knight | No mods', out['rows'][0])
        out = self.grid('C12;H30;S1,0,1;')   # (the stick again: shown, from Meta Knight's centre)
        x, y, shown = self.cursor(out)
        self.assertEqual((shown, round(y, 3)), (1, 2.5))
        self.assertTrue(6.5 < x < 6.7, x)
        self.assertEqual(self.grid('C12;K2;')['level'], '1')

    def test_page_move_on_the_colors(self):
        self.skin('Frieren', color=3)
        self.skin('Other skin', color=6)
        out = self.grid('Tpalutena;zrr')
        self.assertEqual(out['moving'], '1 2')
        self.assertIn('\\24Palutena|.....Mo.|1|palutena', out['rows'][0])
        out = self.grid('Tpalutena;zrra')
        self.assertEqual(out['mods'][0].split(':')[3], 'c05')
        out = self.grid('Tpalutena;d')   # (the second mod's colors: the first one's, moved to color 6, another's)
        self.assertIn('Palutena|.....oM.|0', out['rows'][0])

    def test_shared_color_and_other_page(self):
        self.skin('A', color=3)
        self.skin('B', color=3)
        out = self.grid('Tpalutena;')
        self.assertIn('Palutena|...W....|0', out['rows'][0])
        self.mod('Mario and Luigi', ['fighter/mario/model/body/c00/a', 'fighter/luigi/model/body/c00/a'])
        host = self.mods.parent / 'mods_join' / '.hosts' / '00000000000000a1'
        host.mkdir(parents=True)
        (host / 'host.toml').write_text('host_id = "00000000000000a1"\nname = "Alexis"\nlast = 1\n'
                                        '\n[[mod]]\nid = "1"\nname = "Pikachu Ice"\nfighter = "pikachu"\nactive = true\n'
                                        f'file = "{"ab" * 32} 10 fighter/pikachu/model/body/c04/model.numdlb"\n')
        out = self.grid(f'K{86};')
        self.assertEqual(out['sel'], 'Mario and Luigi')
        self.assertTrue(out['rows'][0].startswith(': \\17'), out['rows'][0])   # (no colors' row)
        self.assertIn("Alexis's room mods", out['rows'][1])
        out = self.grid('K86;dx')
        self.assertIn("Delete the mods kept from Alexis's room?", out['question'])
        self.assertIn('Mario and Luigi', [m.split(':')[0] for m in out['mods']])
        self.assertNotIn('Mario', self.grid()['sums'])

    def test_empty_grid(self):
        out = self.grid()
        self.assertEqual(out['sums'], {})
        self.assertNotIn('A', [f[0] for f in out['rows'][0].split(' | ')[8:]])
        self.assertEqual(out['rows'][1], 'Open Mods Folder: ')
        self.assertEqual(self.grid('dddddddd')['sel'], '(button)')

    def test_pictures_keys(self):
        """The keys of the pictures the view asks gpu_menu_pics.h for: the game's in data.arc, a mod's own"""
        self.mod('Frieren', ['fighter/palutena/model/body/c03/model.numdlb', 'ui/replace/chara/chara_2/chara_2_palutena_03.bntx',
                             'ui/replace/chara/chara_3/chara_3_palutena_03.bntx'])
        self.mod('Model only', ['fighter/palutena/model/body/c05/model.numdlb'])
        out = self.grid('Tpalutena;')
        colors = out['rows'][0].split('|')
        self.assertEqual(colors[2:4], ['0', 'palutena'])
        icons = colors[4:]
        self.assertEqual(len(icons), 8)
        self.assertTrue(icons[3].startswith('file:') and icons[3].endswith('/Frieren/ui/replace/chara/chara_2/chara_2_palutena_03.bntx'), icons[3])
        self.assertEqual(icons[5], '')   # (Model only brings no icon: the game's)
        card = out['rows'][1]
        self.assertTrue(card.endswith('/Frieren/ui/replace/chara/chara_2/chara_2_palutena_03.bntx'), card)
        orig, mine, note, caption = out['rows'][-2].split(' | ')[8].split('|')   # (the preview: the detail's field 8)
        self.assertEqual(orig, 'arc:ui/replace/chara/chara_3/chara_3_palutena_03.bntx')
        self.assertTrue(mine.endswith('/Frieren/ui/replace/chara/chara_3/chara_3_palutena_03.bntx'))
        self.assertEqual((note, caption), ('', 'Palutena - color 4'))
        out = self.grid('Tpalutena;d')   # (Model only: the game's icon, no portrait of its own)
        self.assertTrue(out['rows'][2].endswith('arc:ui/replace/chara/chara_2/chara_2_palutena_05.bntx'), out['rows'][2])
        orig, mine, note, caption = out['rows'][-2].split(' | ')[8].split('|')
        self.assertEqual((mine, note), ('', 'No portrait'))
        self.skin('Pyra', fighter='eflame', color=1)   # (the menus' names: Pyra's eflame_first, a Pokemon's the Trainer's)
        self.skin('Zard', fighter='plizardon', color=2)
        self.assertIn('arc:ui/replace/chara/chara_2/chara_2_eflame_first_01.bntx', self.grid('Teflame;')['rows'][1])
        self.assertIn('arc:ui/replace/chara/chara_2/chara_2_ptrainer_02.bntx', self.grid('Tplizardon;')['rows'][1])

    def test_preview_falls_back_to_preview_png(self):
        self.mod('Model and picture', ['fighter/mario/model/body/c02/model.numdlb'])
        (self.mods / 'Model and picture' / 'Preview.PNG').write_bytes(b'x')   # (its name's case kept)
        orig, mine, note, caption = self.grid('Tmario;')['rows'][-2].split(' | ')[8].split('|')
        self.assertTrue(mine.startswith('file:') and mine.endswith('/Model and picture/Preview.PNG'), mine)
        self.assertEqual((note, caption), ('', 'Mario - color 3'))

    def test_preview_goes_through_the_colors(self):
        self.mod('All of them', [f'fighter/mario/model/body/c{c:02d}/model.numdlb' for c in range(8)])
        caption = lambda out: out['rows'][-2].split(' | ')[8].split('|')[3]
        self.assertEqual(caption(self.grid('Tmario;')), 'Mario - color 1 (1/8)')
        self.assertEqual(caption(self.grid('Tmario;rr')), 'Mario - color 3 (3/8)')
        self.assertEqual(caption(self.grid('Tmario;' + 'r' * 12)), 'Mario - color 8 (8/8)')
        self.assertEqual(caption(self.grid('Tmario;rrl')), 'Mario - color 2 (2/8)')
        self.assertEqual(caption(self.grid('Tmario;rrbTmario;')), 'Mario - color 1 (1/8)')   # (another time: from the first)

    def test_gameplay_warning(self):
        self.mod('Params', ['fighter/mario/param/vl.prc'])
        self.assertEqual(self.run_menu()['gameplay'], 'Online play can desync: Params changes more than looks')
        self.mod('Motion', ['fighter/fox/motion/body/c00/a.nuanmb'])
        self.mod('Skin', ['fighter/fox/model/body/c00/model.numdlb'])
        self.assertEqual(self.run_menu()['gameplay'], 'Online play can desync: 2 mods change more than looks, like Motion')
        (self.mods / 'Motion').rename(self.mods / '.Motion')
        (self.mods / 'Params').rename(self.mods / '.Params')
        self.assertEqual(self.run_menu()['gameplay'], '')   # (disabled ones: no warning)

    def test_clicks_on_the_details_actions_and_colors(self):
        self.skin('Frieren')
        out = self.grid('Tpalutena;A0;')   # (Disable)
        self.assertTrue((self.mods / '.Frieren').is_dir())
        out = self.grid('Tpalutena;A0;')   # (Enable again)
        self.assertTrue((self.mods / 'Frieren').is_dir())
        out = self.grid('Tpalutena;A1;')   # (Move)
        self.assertEqual(out['moving'], '1 0')
        out = self.grid('Tpalutena;A1;A1;')   # (moving: the second is "cancel")
        self.assertEqual(out['moving'], '0 0')
        out = self.grid('Tpalutena;A2;')   # (Share or not)
        self.assertIn('not shared', out['note'])
        out = self.grid('Tpalutena;A3;')   # (Delete: the question, Cancel chosen)
        self.assertEqual(out['dialog'], '1 0')
        out = self.grid('Tpalutena;P6;')   # (a color clicked: moving there, A places it)
        self.assertEqual(out['moving'], '1 3')
        out = self.grid('Tpalutena;P6;A0;')
        self.assertEqual(out['mods'][0].split(':')[3], 'c06')
        out = self.grid('Tmario;A0;')   # (no mods on the page: the action is Open the mods folder)
        self.assertEqual(out['folder'], '1')

if __name__ == '__main__':
    unittest.main()
