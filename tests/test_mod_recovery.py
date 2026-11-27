"""The recovery from a mod that breaks the game (runtime/mod_recovery.c; openspec mod-support,
mod-recovery): tests/mod_recovery_test.c plays sessions over a cache folder, one process each - begun,
running, closed, crashed, frozen - and the next one reads what was left. The menu's question
(runtime/mod_menu.c) through tests/mod_menu_test.c."""
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
LINK = 'fighter/link/model/body/c05/model.numshb'


@unittest.skipUnless(sys.platform.startswith('linux') and CC and (DXVK_INCLUDE / 'directx').is_dir(),
                     'Linux, a C compiler and the Linux toolchain\'s DXVK headers')
class RecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='mod-recovery-test-')
        t = Path(cls.temp.name)
        flags = ['-O1', '-w', '-D_GNU_SOURCE', '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime/linux/include'),
                 '-I', str(DXVK_INCLUDE / 'windows'), '-I', str(DXVK_INCLUDE / 'directx')]
        mods = [str(ROOT / 'runtime' / f) for f in ('mod_scan.c', 'mod_roster.c', 'mod_recovery.c', 'mod_cache.c', 'mod_fetch.c', 'mod_sync.c',
                                                     'mod_zip.c', 'mod_manifest.c', 'mod_sha256.c')]
        for exe, main, extra in (('rec', 'tests/mod_recovery_test.c', ['runtime/mod_recovery.c', 'runtime/mod_scan.c']),
                                 ('menu', 'tests/mod_menu_test.c', None)):
            srcs = [str(ROOT / f) for f in extra] if extra else [str(ROOT / 'runtime/mod_menu.c'), *mods]
            r = subprocess.run([CC, *flags, '-pthread', str(ROOT / main), *srcs, '-lm', '-o', str(t / exe)], capture_output=True, text=True)
            if r.returncode:
                raise AssertionError(f'compiling {main} failed:\n{r.stdout}{r.stderr}')
        cls.rec_exe, cls.menu_exe = t / 'rec', t / 'menu'

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        d = tempfile.TemporaryDirectory(prefix='mod-recovery-run-')
        self.addCleanup(d.cleanup)
        self.root = Path(d.name)
        self.cache = self.root / 'cache' / 'mods'
        self.cache.mkdir(parents=True)
        self.mods = self.root / 'mods'

    def session(self, *steps):
        """One process's session: {key: value} of what it printed"""
        r = subprocess.run([str(self.rec_exe), str(self.cache), *steps], capture_output=True, text=True, timeout=30)
        out = {}
        for line in r.stdout.splitlines():
            for kv in line.split(' '):
                k, _, v = kv.partition('=')
                out[k] = v
        return out

    def last(self):
        return self.session('read')

    def test_good_start_and_normal_close(self):
        self.session('begin', 'A:1,B:2')
        self.assertEqual((self.cache / 'session.txt').read_text().splitlines()[0], 'state = starting')
        self.session('begin', 'A:1,B:2', 'running', 'end')
        self.assertFalse((self.cache / 'session.txt').exists())
        self.assertEqual(self.session('good')['good'], 'A,B')
        self.assertEqual(self.last()['failed'], '0')

    def test_without_mods_nothing_is_written(self):
        self.assertEqual(self.session('begin', '')['begin'], '0')
        self.assertFalse((self.cache / 'session.txt').exists())

    def test_did_not_start(self):
        self.session('begin', 'A:1')   # (the process ends without a word: killed, or closed while loading)
        got = self.last()
        self.assertEqual((got['failed'], got['running']), ('1', '0'))
        self.assertFalse((self.cache / 'session.txt').exists())   # (read once: the next start sees nothing)
        self.assertEqual(self.last()['failed'], '0')

    def test_crash_loading_a_fight_names_the_mod_and_color(self):
        self.session('begin', 'A:1,LinkOscuro:2', 'running', 'file', 'A', 'fighter/mario/model/body/c00/a', 'file', 'LinkOscuro', LINK, 'crash')
        got = self.last()
        self.assertEqual((got['failed'], got['running'], got['suspect'], got['path']), ('2', '1', 'LinkOscuro', LINK))
        self.session('begin', 'A:1', 'fatal')
        self.assertEqual(self.last()['failed'], '3')

    def test_no_file_read(self):
        self.session('begin', 'A:1', 'running', 'end')   # (A worked)
        self.session('begin', 'A:1', 'crash')
        got = self.last()
        self.assertEqual((got['suspect'], got['suspect_new'], got['new']), ('', '0', ''))   # (probably not the mods)
        self.session('begin', 'A:1,C:3', 'crash')
        got = self.last()
        self.assertEqual((got['suspect'], got['suspect_new'], got['new']), ('C', '1', 'C'))   # (the new one)
        self.session('begin', 'A:2,C:3,D:4', 'crash')   # (A changed, C and D new)
        got = self.last()
        self.assertEqual((got['suspect'], got['suspect_new'], got['new']), ('', '1', 'A,C,D'))

    def test_frozen(self):
        self.session('begin', 'A:1', 'running', 'frozen')
        self.assertEqual(self.last()['failed'], '4')

    def test_the_last_of_eight_files_read(self):
        steps = []
        for k in range(11):
            steps += ['file', f'M{k}', f'fighter/mario/model/body/c0{k % 8}/f{k}']
        self.session('begin', 'M0:1', *steps, 'crash')
        text = (self.cache / 'session.txt').read_text()
        self.assertEqual(text.count('read = '), 8)
        self.assertIn('read = M3\t', text)
        self.assertNotIn('read = M2\t', text)
        self.assertEqual(self.last()['suspect'], 'M10')

    def test_two_failures_in_a_row_then_without_mods(self):
        self.session('begin', 'A:1,B:2', 'crash')
        got = self.session('read', 'begin', 'A:1,B:2', 'crash')
        self.assertEqual(got['fails'], '1')
        got = self.session('read', 'begin', 'A:1,B:2')
        self.assertEqual((got['fails'], got['no_mods'], got['begin']), ('2', '1', '1'))   # (without mods, they stay enabled)
        self.assertFalse((self.cache / 'session.txt').exists())
        self.assertEqual(self.last()['failed'], '0')   # (the next start: with them again)

    def test_a_choice_or_other_mods_lift_the_rule(self):
        self.session('begin', 'A:1', 'crash')
        self.session('read', 'begin', 'A:1', 'crash')
        self.assertEqual(self.session('read', 'chosen', 'begin', 'A:1')['begin'], '0')   # (the menu's choice: with them)
        self.session('begin', 'A:1', 'crash')
        self.session('read', 'begin', 'A:1', 'crash')
        self.assertEqual(self.session('read', 'begin', 'A:2')['begin'], '0')   # (other mods: counted again)
        self.assertIn('fails = 0', (self.cache / 'session.txt').read_text())

    # ---- the menu's question ----

    def mod(self, name, path=LINK):
        p = self.mods / name / path
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b'x' * 16)

    def menu(self, script='', **env):
        base = {k: v for k, v in os.environ.items() if not k.startswith('SSBU_')}
        r = subprocess.run([str(self.menu_exe), str(self.mods), script], capture_output=True, text=True, timeout=30,
                           env={**base, 'TEST_RECOVERY_DIR': str(self.cache), **env})
        self.assertEqual(r.returncode, 0, r.stderr)
        return dict(line.split('=', 1) for line in r.stdout.splitlines() if '=' in line and not line.startswith('row'))

    def crashed_on_link(self):
        self.session('begin', 'Clark:1,LinkOscuro:2', 'running', 'file', 'LinkOscuro', LINK, 'crash')

    def test_question_disable_the_suspect(self):
        self.mod('Clark', 'fighter/mario/model/body/c00/model.numdlb')
        self.mod('LinkOscuro')
        self.crashed_on_link()
        out = self.menu()
        self.assertEqual((out['dialog'][0], out['tile']), ('2', '2:Link'))
        self.assertIn('The game crashed', out['question'])
        self.assertIn('crashed while playing with mods. It was loading LinkOscuro (Link color 6)', out['question'])
        self.assertIn('Disable LinkOscuro|Back to the mods that last worked|Start without mods this time|Ignore', out['question'])
        self.crashed_on_link()
        out = self.menu('a')
        self.assertTrue((self.mods / '.LinkOscuro').is_dir() and (self.mods / 'Clark').is_dir())
        self.assertIn('LinkOscuro disabled', out['note'])

    def test_question_back_to_what_worked_without_mods_and_ignore(self):
        self.mod('Clark', 'fighter/mario/model/body/c00/model.numdlb')
        self.session('begin', 'Clark:1', 'running', 'end')   # (Clark worked)
        for name in ('New1', 'New2', 'New3'):
            self.mod(name, f'fighter/fox/model/body/c0{name[-1]}/model.numdlb')
        self.session('begin', 'Clark:1,New1:1,New2:1,New3:1', 'crash')
        out = self.menu()
        self.assertIn('3 mods are new since the last start that worked', out['question'])
        self.assertTrue(out['question'].endswith('Back to the mods that last worked|Start without mods this time|Ignore'))   # (no one to disable)
        self.session('begin', 'Clark:1,New1:1,New2:1,New3:1', 'crash')
        out = self.menu('a')
        self.assertEqual(sorted(p.name for p in self.mods.iterdir()), ['.New1', '.New2', '.New3', 'Clark'])
        for name in ('New1', 'New2', 'New3'):
            (self.mods / f'.{name}').rename(self.mods / name)
        self.session('begin', 'Clark:1', 'crash')
        out = self.menu('da')   # (without mods: nothing touched)
        self.assertEqual(out['no_mods'], '1')
        self.assertEqual(sorted(p.name for p in self.mods.iterdir()), ['Clark', 'New1', 'New2', 'New3'])
        self.session('begin', 'Clark:1', 'crash')
        out = self.menu('b')   # (ignore)
        self.assertEqual((out['dialog'][0], out['no_mods']), ('0', '0'))
        self.assertEqual(sorted(p.name for p in self.mods.iterdir()), ['Clark', 'New1', 'New2', 'New3'])

    def test_question_says_probably_not_the_mods_and_the_two_failures(self):
        self.mod('Clark', 'fighter/mario/model/body/c00/model.numdlb')
        self.session('begin', 'Clark:1', 'running', 'end')
        self.session('begin', 'Clark:1', 'crash')
        self.session('read', 'begin', 'Clark:1', 'crash')
        out = self.menu()
        self.assertIn('no mod is new: probably not the mods', out['question'])
        self.assertIn('It failed twice in a row with these mods', out['question'])

    def test_safe_mode(self):
        self.mod('Clark', 'fighter/mario/model/body/c00/model.numdlb')
        out = self.menu('', TEST_RECOVERY_SAFE='1')
        self.assertTrue(out['question'].startswith('Safe mode'))
        self.assertTrue(out['question'].endswith('\t'.replace('\t', ' | ') + 'Start without mods this time|Ignore'), out['question'])
        self.assertEqual(out['dialog'], '2 0')   # (without mods: chosen at first)
        self.assertEqual(self.menu('a', TEST_RECOVERY_SAFE='1')['no_mods'], '1')
        self.assertEqual(self.menu('', )['dialog'][0], '0')   # (nothing failed, no safe mode: no question)


if __name__ == '__main__':
    unittest.main()
