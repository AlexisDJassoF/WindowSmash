"""tools/build_game.py's linux target and tools/gamefiles.py's emulator folders on Linux: the
default target, the refusal of --target windows on Linux, and where the folders are looked for."""
from pathlib import Path
import os
import runpy
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import gamefiles  # noqa: E402

BUILD = runpy.run_path(str(ROOT / 'tools/build_game.py'), run_name='build_game_linux_reference')
ON_LINUX = sys.platform.startswith('linux')


class TargetTests(unittest.TestCase):
    def test_default_is_this_system(self):
        self.assertEqual(BUILD['default_target'](), 'linux' if ON_LINUX else 'windows')

    @unittest.skipUnless(ON_LINUX, 'a Linux host')
    def test_windows_target_refused_on_linux(self):
        with mock.patch.object(sys, 'argv', ['build_game.py', '--target', 'windows', '--check']):
            out = []
            with mock.patch.dict(BUILD['main'].__globals__, {'log': out.append}):
                self.assertEqual(BUILD['main'](), 1)
        self.assertIn('made on Windows only', ''.join(out))

    def test_shown_uses_this_systems_separator(self):
        self.assertEqual(BUILD['shown']('dump\\modules\\romfs.cfg'), os.path.join('dump', 'modules', 'romfs.cfg'))


class EmulatorFolderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='emulator-test-')
        self.addCleanup(self.temp.cleanup)
        self.home = self.temp.name

    def folders(self, env=None):
        return gamefiles.emulator_folders(env=env or {}, home=self.home)

    def test_xdg_defaults(self):
        found = self.folders()
        share, config = os.path.join(self.home, '.local/share'), os.path.join(self.home, '.config')
        self.assertEqual(found['keys'][0], os.path.join(share, 'yuzu/keys'))
        self.assertIn(os.path.join(config, 'Ryujinx/system'), found['keys'])
        self.assertEqual(found['keys'][-1], os.path.join(self.home, '.switch'))
        self.assertIn(os.path.join(share, 'eden/nand/system/Contents/registered'), found['firmware'])
        self.assertIn(os.path.join(config, 'Ryujinx/bis/user/Contents/registered'), found['game'])

    def test_xdg_variables(self):
        found = self.folders({'XDG_DATA_HOME': '/data', 'XDG_CONFIG_HOME': '/conf'})
        self.assertEqual(found['keys'][0], '/data/yuzu/keys')
        self.assertIn('/conf/Ryujinx/system', found['keys'])

    def test_flatpak(self):
        app = Path(self.home, '.var/app/org.ryujinx.Ryujinx/config/Ryujinx/system')
        app.mkdir(parents=True)
        Path(self.home, '.var/app/org.yuzu_emu.yuzu/data').mkdir(parents=True)
        found = self.folders()
        self.assertIn(str(app), found['keys'])
        self.assertIn(os.path.join(self.home, '.var/app/org.yuzu_emu.yuzu/data/yuzu/keys'), found['keys'])

    @unittest.skipUnless(ON_LINUX, 'a Linux host')
    def test_ryujinx_keys_offered(self):
        keys = Path(self.home, '.config/Ryujinx/system')
        keys.mkdir(parents=True)
        (keys / 'prod.keys').write_text('')
        with mock.patch.dict(os.environ, {'HOME': self.home}, clear=False), \
                mock.patch.object(os.path, 'expanduser', lambda p: p.replace('~', self.home, 1)):
            for var in ('XDG_DATA_HOME', 'XDG_CONFIG_HOME'):
                os.environ.pop(var, None)
            self.assertEqual(BUILD['default_folder']('keys'), str(keys))
            self.assertEqual(BUILD['default_folder']('game'), os.path.join(self.home, 'MyDump'))


if __name__ == '__main__':
    unittest.main()
