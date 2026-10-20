"""The start-up settings menu's model (runtime/gpu_menu.c) without a window: tests/gpu_menu_test.c,
built here with the host's C compiler, plays scripts of keys, clicks and time against it."""
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
OPEN, PLAY, KEEP = 0, 1, 2


@unittest.skipUnless(sys.platform.startswith('linux') and CC and (DXVK_INCLUDE / 'directx').is_dir(),
                     'Linux, a C compiler and the Linux toolchain\'s DXVK headers')
class MenuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='menu-test-')
        cls.exe = Path(cls.temp.name) / 'gpu_menu_test'
        flags = ['-O1', '-w', '-D_GNU_SOURCE', '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime/linux/include'),
                 '-I', str(DXVK_INCLUDE / 'windows'), '-I', str(DXVK_INCLUDE / 'directx')]
        r = subprocess.run([CC, *flags, str(ROOT / 'tests/gpu_menu_test.c'), str(ROOT / 'runtime/gpu_menu.c'),
                            str(ROOT / 'runtime/gpu_scale.c'), '-lm', '-o', str(cls.exe)],
                           capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(f'compiling the test failed:\n{r.stdout}{r.stderr}')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_menu(self, script, **env):
        r = subprocess.run([str(self.exe), script], capture_output=True, text=True, env={**os.environ, **env}, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        return dict(kv.split('=', 1) for kv in r.stdout.strip().split('|'))

    def test_starts_on_play_and_enter_plays(self):
        out = self.run_menu('a')
        self.assertEqual((int(out['state']), out['row']), (PLAY, 'Play'))

    def test_change_mode_and_play(self):
        out = self.run_menu('dra')   # down from Play wraps to Mode; right; Mode is not Play, so a moves down
        self.assertEqual(out['mode'], 'Handheld (720p)')
        self.assertEqual(int(out['state']), OPEN)
        out = self.run_menu('dr' + 'd' * 5 + 'a')   # on down to Play (Sharpness hidden: 5 rows)
        self.assertEqual((int(out['state']), out['mode']), (PLAY, 'Handheld (720p)'))

    def test_sharpness_only_with_fsr(self):
        out = self.run_menu('ddd')   # Mode, Resolution scale, then Upscaling
        self.assertEqual((out['row'], out['sharp_shown']), ('Upscaling', '0'))
        out = self.run_menu('dddrd')   # FSR on: the next row is Sharpness
        self.assertEqual((out['upscale'], out['row'], out['sharp_shown']), ('FSR', 'Sharpness', '1'))
        out = self.run_menu('dddrdrrr')
        self.assertEqual(out['sharpness'], '0.5')
        out = self.run_menu('dddrd' + 'l' * 30)
        self.assertEqual(out['sharpness'], '0.0')

    def test_vsync_cycles(self):
        self.assertEqual(self.run_menu('uur')['vsync'], 'VRR')        # (up from Play: Window, then VSync; Off -> VRR)
        self.assertEqual(self.run_menu('uul')['vsync'], 'On')
        self.assertEqual(self.run_menu('uu', TEST_PACING='2')['vsync'], 'Display')
        self.assertEqual(self.run_menu('uul', TEST_PACING='2')['vsync'], 'On')    # display paces as Off: stepped from there
        self.assertEqual(self.run_menu('uur', TEST_PACING='2')['vsync'], 'VRR')

    def test_window_toggles(self):
        self.assertEqual(self.run_menu('ur')['window'], 'Fullscreen')

    def test_untouched_starts_itself(self):
        self.assertEqual(int(self.run_menu('t9.9')['state']), OPEN)
        self.assertEqual(int(self.run_menu('t10')['state']), KEEP)

    def test_touched_waits(self):
        self.assertEqual(int(self.run_menu('ut60')['state']), OPEN)

    def test_back_keeps_display_ini(self):
        out = self.run_menu('drb')
        self.assertEqual((int(out['state']), out['mode']), (KEEP, 'Docked (1080p)'))

    def test_locked_row_does_not_change(self):
        out = self.run_menu('dr', TEST_LOCKED='2')   # LOCK_MODE
        self.assertEqual(out['mode'], 'Docked (1080p) (set by SSBU_MODE)')

    def test_resolution_scale(self):
        self.assertEqual(self.run_menu('')['scale'], '100% (1920 x 1080)')
        out = self.run_menu('ddl')   # Resolution scale, one step down
        self.assertEqual((out['row'], out['scale'], out['fsr_hint']), ('Resolution scale', '75% (1440 x 810)', '1'))
        self.assertEqual(self.run_menu('ddll')['scale'], '50% (960 x 540)')
        self.assertEqual(self.run_menu('ddlllll')['scale'], '50% (960 x 540)')   # (no wrapping)
        self.assertEqual(self.run_menu('ddrrrr')['scale'], '200% (3840 x 2160)')
        self.assertEqual(self.run_menu('drdl')['scale'], '75% (960 x 540)')   # handheld: of 720p
        self.assertEqual(self.run_menu('ddldr')['fsr_hint'], '0')   # (FSR on: no hint)
        self.assertEqual(self.run_menu('ddl', TEST_LOCKED='16')['scale'], '100% (1920 x 1080) (set by SSBU_SCALE)')

    def test_clicks(self):
        # row 0 (Mode) at y 100-140: its value's right half steps it; Play is row 5 at y 300-340
        out = self.run_menu('c500,120')
        self.assertEqual((out['row'], out['mode']), ('Mode', 'Handheld (720p)'))
        out = self.run_menu('c100,120')   # the label: selects only
        self.assertEqual((out['row'], out['mode']), ('Mode', 'Docked (1080p)'))
        self.assertEqual(int(self.run_menu('c300,360')['state']), PLAY)   # (Play: row 6, y 340-380)


if __name__ == '__main__':
    unittest.main()
