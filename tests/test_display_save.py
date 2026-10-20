"""display.ini rewritten by the start-up menu (runtime/gpu_display_ini.c): tests/display_save_test.c,
built here with the host's C compiler, rewrites made-up files - the user's comments, keys it does
not know, keys an older file lacks, CRLF lines, keys in capitals, a comment after a value."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
CC = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')

DEFAULTS = '''; display.ini: the defaults.

; window = windowed | fullscreen
window = windowed
width = 1280

; mode = docked | handheld: the console the game believes it runs on.
;   handheld: 1280 x 720.
mode = docked
; upscale = linear | fsr
upscale = linear
'''


@unittest.skipUnless(CC, 'a C compiler')
class DisplaySaveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='display-save-')
        cls.exe = Path(cls.temp.name) / 'display_save_test'
        r = subprocess.run([CC, '-O1', '-w', '-I', str(ROOT / 'runtime'), str(ROOT / 'tests/display_save_test.c'),
                            str(ROOT / 'runtime/gpu_display_ini.c'), '-o', str(cls.exe)], capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(f'compiling the test failed:\n{r.stdout}{r.stderr}')
        cls.defaults = Path(cls.temp.name) / 'defaults.ini'
        cls.defaults.write_text(DEFAULTS)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def rewrite(self, text, **values):
        f = Path(self.temp.name) / 'display.ini'
        f.write_bytes(text.encode())
        r = subprocess.run([str(self.exe), str(f), str(self.defaults), *(f'{k}={v}' for k, v in values.items())],
                           capture_output=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.decode()

    def test_value_replaced_in_place(self):
        text = '; my own note\nwindow = windowed\n  mode   =   docked   ; my comment\nwidth = 1600\nfoo = bar\n'
        out = self.rewrite(text, mode='handheld')
        self.assertEqual(out, '; my own note\nwindow = windowed\n  mode   =   handheld   ; my comment\nwidth = 1600\nfoo = bar\n')

    def test_missing_key_added_with_its_comment(self):
        out = self.rewrite('window = fullscreen\n', mode='handheld')
        self.assertEqual(out, 'window = fullscreen\n\n; mode = docked | handheld: the console the game believes it runs on.\n'
                              ';   handheld: 1280 x 720.\nmode = handheld\n')

    def test_several_keys_and_no_final_newline(self):
        out = self.rewrite('upscale = linear', upscale='fsr', mode='handheld')
        self.assertTrue(out.startswith('upscale = fsr\n\n; mode = docked'), out)
        self.assertTrue(out.endswith('mode = handheld\n'), out)

    def test_crlf_and_capitals(self):
        out = self.rewrite('Window = windowed\r\nMODE = docked\r\n', mode='handheld')
        self.assertEqual(out, 'Window = windowed\r\nMODE = handheld\r\n')

    def test_comments_and_sections_untouched(self):
        text = '; mode = docked (an example in a comment)\n[section]\n#mode = docked\nmode = docked\n'
        out = self.rewrite(text, mode='handheld')
        self.assertEqual(out, '; mode = docked (an example in a comment)\n[section]\n#mode = docked\nmode = handheld\n')

    def test_every_occurrence(self):   # (the last one wins when read: all must agree)
        out = self.rewrite('mode = docked\nmode = docked\n', mode='handheld')
        self.assertEqual(out, 'mode = handheld\nmode = handheld\n')

    def test_key_not_in_defaults(self):
        out = self.rewrite('window = windowed\n', sharpness='0.5')
        self.assertEqual(out, 'window = windowed\n\nsharpness = 0.5\n')

    def test_nothing_to_change(self):
        text = '; note\nmode = docked\n'
        self.assertEqual(self.rewrite(text), text)


if __name__ == '__main__':
    unittest.main()
