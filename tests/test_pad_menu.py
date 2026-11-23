"""The start-up menu's Controls page (runtime/pad_menu.c) without a window: tests/pad_menu_test.c,
built here with the host's C compiler, plays scripts against it and controllers.ini."""
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


@unittest.skipUnless(sys.platform.startswith('linux') and CC and (DXVK_INCLUDE / 'directx').is_dir(),
                     'Linux, a C compiler and the Linux toolchain\'s DXVK headers')
class PadMenuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='pad-menu-test-')
        cls.exe = Path(cls.temp.name) / 'pad_menu_test'
        flags = ['-O1', '-w', '-D_GNU_SOURCE', '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime/linux/include'),
                 '-I', str(DXVK_INCLUDE / 'windows'), '-I', str(DXVK_INCLUDE / 'directx')]
        r = subprocess.run([CC, *flags, str(ROOT / 'tests/pad_menu_test.c'), str(ROOT / 'runtime/pad_menu.c'), '-o', str(cls.exe)],
                           capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(f'compiling the test failed:\n{r.stdout}{r.stderr}')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(prefix='pad-menu-')
        self.ini = Path(self.dir.name) / 'controllers.ini'

    def tearDown(self):
        self.dir.cleanup()

    def run_menu(self, script):
        r = subprocess.run([str(self.exe), script], capture_output=True, text=True, timeout=30,
                           env={**os.environ, 'SSBU_CONTROLLERS': str(self.ini)})
        self.assertEqual(r.returncode, 0, r.stderr)
        return dict(kv.split('=', 1) for kv in r.stdout.rstrip('\n').split('|'))

    def test_defaults(self):
        out = self.run_menu('')
        self.assertEqual((out['row'], out['layout'], out['deadzone'], out['rumble']),
                         ('Face buttons', 'By position (as a Switch)', 'Left 0%, right 0%', '100%'))
        self.assertEqual((out['Button A'], out['Button B'], out['ZR'], out['Plus (+)']), ('B', 'A', 'RT', 'Menu'))   # (Xbox names, by position)

    def test_layout_labels_and_ps_names(self):
        out = self.run_menu('r')
        self.assertEqual((out['layout'], out['Button A'], out['Button B']), ('By the printed letters', 'A', 'B'))
        out = self.run_menu('P')
        self.assertEqual((out['Button A'], out['Button B'], out['ZL'], out['L']), ('Circle', 'Cross', 'L2', 'L1'))

    def test_dead_zones_page(self):
        out = self.run_menu('da')   # (Stick dead zones: its own page)
        self.assertEqual((out['row'], out['dead_zones'], out['dz_shown'], out['a_shown'], out['takes_back']),
                         ('Left stick', '1', '1', '0', '1'))
        out = self.run_menu('darrrdrr')   # (each stick its own)
        self.assertEqual((out['left'], out['right'], out['deadzone']), ('15%', '10%', 'Left 15%, right 10%'))
        self.assertEqual(self.run_menu('da' + 'r' * 20)['left'], '50%')   # (at most 50)
        self.assertEqual(self.run_menu('dr')['dead_zones'], '1')   # (> opens it too)
        out = self.run_menu('darb')   # (back: the rest of the page, the change kept)
        self.assertEqual((out['row'], out['dead_zones'], out['deadzone'], out['takes_back']), ('Stick dead zones', '0', 'Left 5%, right 0%', '0'))
        out = self.run_menu('daddddda')   # (Back's row)
        self.assertEqual((out['row'], out['dead_zones']), ('Stick dead zones', '0'))

    def test_stick_test(self):
        test = 'dadddda'   # (Test the sticks: started)
        out = self.run_menu(test)
        self.assertEqual((out['row'], out['testing'], out['test']), ('Test the sticks', '1', 'Testing - move the sticks'))
        self.assertEqual(self.run_menu(test + 'a')['testing'], '0')   # (Enter again: stopped)
        out = self.run_menu(test + 'b')   # (Back: the test stopped, still on the page)
        self.assertEqual((out['testing'], out['dead_zones']), ('0', '1'))
        out = self.run_menu(test + 'uuuur')   # (the dead zone changed while testing)
        self.assertEqual((out['testing'], out['left']), ('1', '5%'))
        self.assertEqual(self.run_menu(test + 'bb')['dead_zones'], '0')

    def test_snapback_fix(self):
        out = self.run_menu('dadd')
        self.assertEqual((out['row'], out['snap_left'], out['snap_right']), ('Left snapback', 'Off', 'Off'))   # (off by default)
        out = self.run_menu('daddr' + 'da')   # (> on the left one, Enter on the right one)
        self.assertEqual((out['snap_left'], out['snap_right']), ('On - the bounce ignored', 'On - the bounce ignored'))
        self.run_menu('daddrW')
        text = self.ini.read_text()
        self.assertIn('snapback_left = on\nsnapback_right = off\n', text)
        self.assertEqual(self.run_menu('')['snap_left'], 'On - the bounce ignored')
        self.ini.write_text('[device default]\nsnapback_right = 1\n')
        out = self.run_menu('')
        self.assertEqual((out['snap_left'], out['snap_right']), ('Off', 'On - the bounce ignored'))

    def test_snapback_filter(self):
        flags = ['-O1', '-w', '-I', str(ROOT / 'runtime')]
        exe = Path(self.dir.name) / 'pad_snap_test'
        r = subprocess.run([CC, *flags, str(ROOT / 'tests/pad_snap_test.c'), '-lm', '-o', str(exe)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)

    def test_rumble(self):
        self.assertEqual(self.run_menu('ddll')['rumble'], '80%')

    def test_assign_by_pressing(self):
        out = self.run_menu('ddda')   # (Button A: waiting)
        self.assertEqual((out['row'], out['listening'], out['Button A']), ('Button A', '1', 'Press a button...'))
        out = self.run_menu('dddak0')   # (the bottom button)
        self.assertEqual((out['listening'], out['Button A']), ('0', 'A  *'))
        out = self.run_menu('dddadddd' + 'akT')   # (ZR: the right trigger again - as the layout has it, not marked)
        self.assertEqual(out['ZR'], 'RT')
        out = self.run_menu('dddab')   # (Back: nothing assigned)
        self.assertEqual((out['listening'], out['Button A']), ('0', 'B'))

    def test_cycle(self):
        self.assertEqual(self.run_menu('dddr')['Button A'], 'X  *')   # (after the right face button: the left one)
        self.assertEqual(self.run_menu('dddrl')['Button A'], 'B')

    def test_reset(self):
        out = self.run_menu('dddak0' + 'd' * 12 + 'a')
        self.assertEqual((out['row'], out['Button A']), ('Reset to defaults', 'B'))

    def test_play(self):
        self.assertEqual(self.run_menu('u' + 'a')['plays'], '1')   # (up from Face buttons: around to Play)
        out = self.run_menu('Nua')   # (no Play row: up from Face buttons is Reset)
        self.assertEqual((out['row'], out['plays']), ('Reset to defaults', '0'))

    def test_saved_keeps_the_rest(self):
        self.ini.write_text('; mine\n[general]\nkeyboard_player = 2\n\n[device default]\n; note\nlayout = positional\nZL = leftshoulder\n\n[device ps5]\nrumble = 60\n')
        out = self.run_menu('')
        self.assertEqual(out['ZL'], 'LB  *')
        self.run_menu('rdarrbdddak1W')   # (labels, left dead zone 10, Button B: the right face button)
        text = self.ini.read_text()
        self.assertIn('; mine\n[general]\nkeyboard_player = 2\n', text)
        self.assertIn('[device ps5]\nrumble = 60\n', text)
        self.assertIn('layout = labels\n', text)
        self.assertIn('deadzone_left = 10\ndeadzone_right = 0\n', text)
        self.assertIn('ZL = leftshoulder\n', text)
        self.assertIn('; note\n', text)
        out = self.run_menu('')
        self.assertEqual((out['layout'], out['deadzone'], out['ZL']), ('By the printed letters', 'Left 10%, right 0%', 'LB  *'))

    def test_saved_from_nothing(self):
        self.run_menu('dadrW')   # (the right stick's dead zone 5%)
        text = self.ini.read_text()
        self.assertIn('[general]\nkeyboard_player = 1\n', text)   # (the defaults written first)
        self.assertIn('deadzone_right = 5\n', text.split('[device default]')[1].split('[device ps5]')[0])
        self.assertNotIn('\ndeadzone = ', text)   # (the old key: one per stick now)
        self.assertEqual(self.run_menu('')['deadzone'], 'Left 0%, right 5%')
        self.assertEqual(text.count('[device default]'), 1)

    def test_old_single_dead_zone(self):
        self.ini.write_text('[device default]\ndeadzone = 20\ndeadzone_right = 30\n')
        self.assertEqual(self.run_menu('')['deadzone'], 'Left 20%, right 30%')


if __name__ == '__main__':
    unittest.main()
