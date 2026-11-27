"""A room's mods for who joins (runtime/mod_room.c; openspec change netplay-mod-sync): the dialog - what it
lists and says, Play without them chosen when it opens, the mode, Menu -, the download and what this
start then loads, the next visit applied without asking, netplay.ini's never and always, a host that
is an older game, and the online lobby's way: over the game, behind it, then Restart and rejoin.
Built with tests/mod_room_test.c (host and guest in one process)."""
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

SKIN = 'fighter/pikachu/model/body/c04/model.numdlb'
PARAM = 'fighter/mario/param/fighter_param.prc'
# mod_room.h's
IDLE, ASKING, DIALOG, FETCHING, AFTER, READY = range(6)
EV_NONE, EV_PLAY, EV_MENU, EV_RESTART = range(4)


@unittest.skipUnless(sys.platform.startswith('linux') and CC and (DXVK_INCLUDE / 'directx').is_dir(),
                     'Linux, a C compiler and the Linux toolchain\'s DXVK headers')
class RoomTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='mod-room-test-')
        cls.exe = Path(cls.temp.name) / 'mod_room_test'
        flags = ['-O1', '-g', '-w', '-D_GNU_SOURCE', '-pthread', '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime/linux/include'),
                 '-I', str(DXVK_INCLUDE / 'windows'), '-I', str(DXVK_INCLUDE / 'directx')]
        srcs = ['tests/mod_room_test.c'] + [f'runtime/{f}' for f in ('mod_room.c', 'mod_cache.c', 'mod_fetch.c', 'mod_sync.c', 'mod_zip.c',
                                                                       'mod_manifest.c', 'mod_sha256.c', 'mod_scan.c', 'mod_menu.c', 'mod_roster.c', 'mod_recovery.c', 'net_menu.c')]
        r = subprocess.run([CC, *flags, *[str(ROOT / s) for s in srcs], '-lm', '-o', str(cls.exe)], capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(f'compiling the test failed:\n{r.stdout}{r.stderr}')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        d = tempfile.TemporaryDirectory(prefix='mod-room-run-')
        self.addCleanup(d.cleanup)
        self.dir = Path(d.name)
        self.host = self.dir / 'host' / 'mods'
        self.mine = self.dir / 'me' / 'mods'
        self.host.mkdir(parents=True)
        self.mine.mkdir(parents=True)

    def host_mod(self, folder, files):
        for path, data in files.items():
            p = self.host / folder / path
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)

    def run_room(self, script, *opts):
        base = {k: v for k, v in os.environ.items() if not k.startswith('SSBU_')}
        r = subprocess.run([str(self.exe), str(self.host), str(self.mine), script, *opts], capture_output=True, text=True, env=base,
                           timeout=60, cwd=self.dir)
        self.assertEqual(r.returncode, 0, r.stderr)
        out = {'steps': [], 'lines': [], 'buttons': [], 'log': r.stderr}
        for line in r.stdout.splitlines():
            key, _, rest = line.partition(' ')
            if key == 'stage':
                w = rest.split()
                out['steps'].append((int(w[0]), int(w[2]), int(w[4])))
            elif key == 'line':
                out['lines'].append((rest[0], rest[2:]))
            elif key == 'button':
                out['buttons'].append(rest.split(' ', 1)[1])
            else:
                out[key] = rest
        return out

    def skins(self):
        self.host_mod('Pikachu Ice', {SKIN: b'ice' * 2000})
        self.host_mod('Physics', {PARAM: b'prc' * 100})

    def test_dialog_opens_on_play_without_them(self):
        self.skins()
        out = self.run_room('w')
        self.assertEqual(out['steps'][-1], (DIALOG, EV_NONE, 1))
        self.assertIn("Alexis's room uses 2 mods", out['title'])
        texts = dict((t.split(' - ')[0], (k, t)) for k, t in out['lines'])
        self.assertEqual(texts['Pikachu Ice (Pikachu, color 5)'][0], '+')
        self.assertEqual(texts['Physics (Mario)'][0], '!')   # (it changes how the game plays)
        self.assertIn('changes how the game plays', texts['Physics (Mario)'][1])
        self.assertEqual(out['buttons'], ['Download and apply', 'Play without them', 'Menu'])
        self.assertIn('Mirror You play and see exactly what the host has.', out['mode'])
        self.assertTrue(any('Downloaded from the host' in t for _, t in out['lines']))

    def test_play_without_them(self):
        self.skins()
        out = self.run_room('wb')   # (Back: as Play without them)
        self.assertEqual(out['steps'][-1][:2], (READY, EV_PLAY))
        self.assertEqual(out['env'], '')
        self.assertFalse((self.dir / 'me' / 'mods_join' / '.hosts').exists())
        out = self.run_room('wa')   # (Accept on the button chosen when it opens: the same)
        self.assertEqual(out['steps'][-1][:2], (READY, EV_PLAY))
        self.assertEqual(out['env'], '')

    def test_download_and_apply_then_next_time_without_asking(self):
        self.skins()
        out = self.run_room('wlaw')   # (left: Download and apply)
        self.assertEqual([s for s, _, _ in out['steps']], [DIALOG, DIALOG, FETCHING, READY])
        self.assertEqual(out['env'], '0000000000001234:mirror')
        self.assertTrue((self.dir / 'me' / 'mods_join' / '.hosts' / '0000000000001234' / 'host.toml').exists())
        self.assertEqual(len(list((self.dir / 'me' / 'mods_join' / '.blobs').iterdir())), 2)
        self.assertTrue((self.dir / 'me' / 'mods_join' / 'Alexis' / 'Pikachu Ice' / SKIN).exists())   # (the folder to look at)
        out = self.run_room('w')   # (the same mods: applied without the dialog)
        self.assertEqual(out['steps'][-1][0], READY)
        self.assertEqual(out['env'], '0000000000001234:mirror')
        self.assertIn('2 mods as last time', out['note'])

    def test_overlay_chosen_in_the_dialog(self):
        self.skins()
        out = self.run_room('wurdla')   # (up to the mode, right: Overlay; down, left: Download and apply)
        self.assertEqual(out['steps'][1][2], -1)
        out_mode = self.run_room('wur')['mode']
        self.assertIn('Overlay You play with the host', out_mode)
        self.assertEqual(self.run_room('wurdlaw')['env'], '0000000000001234:overlay')

    def test_menu(self):
        self.skins()
        out = self.run_room('wra')   # (right: Menu)
        self.assertEqual(out['steps'][-1][:2], (IDLE, EV_MENU))
        self.assertEqual(out['env'], '')
        self.assertEqual(self.run_room('w2')['steps'][-1][1], EV_MENU)   # (clicked)

    def test_never_and_always(self):
        self.skins()
        out = self.run_room('t', 'never')
        self.assertEqual(out['steps'][-1][0], READY)
        self.assertNotIn('room manifest', out['log'])   # (not even asked)
        out = self.run_room('ww', 'always')
        self.assertEqual(out['steps'][-1][0], READY)
        self.assertEqual(out['env'], '0000000000001234:mirror')
        self.assertEqual(self.run_room('ww', 'always', 'overlay')['env'], '0000000000001234:overlay')

    def test_host_without_mods_or_older(self):
        out = self.run_room('w')
        self.assertEqual(out['steps'][-1][0], READY)
        self.assertEqual(out['env'], '')
        self.skins()
        out = self.run_room('w', 'old')
        self.assertEqual(out['steps'][-1][0], READY)   # (3 s without an answer: as before)
        self.assertEqual(out['env'], '')

    def test_moved_mod_applied_without_asking(self):
        self.host_mod('Link Dark', {'fighter/link/model/body/c05/model.numdlb': b'dark' * 100})
        self.run_room('wlaw')
        (self.host / 'windowsmash.toml').write_text('[mod."Link Dark"]\nlink.c05 = "c06"\n')
        out = self.run_room('w')
        self.assertEqual(out['steps'][-1][0], READY)
        self.assertIn('1 moved', out['note'])
        toml = (self.dir / 'me' / 'mods_join' / '.hosts' / '0000000000001234' / 'host.toml').read_text()
        self.assertIn('to = [-1, -1, -1, -1, -1, 6, -1, -1]', toml)

    def test_lobby(self):
        self.skins()
        out = self.run_room('wla', 'lobby')   # (the room entered: the dialog over the game)
        self.assertEqual([s for s, _, _ in out['steps']], [DIALOG, DIALOG, FETCHING])
        self.assertEqual(out['steps'][-1][1], EV_PLAY)   # (Download and apply closes it: behind the game)
        self.assertEqual(out['show'], '2')               # (a line over the game, not a panel)
        # the lobby: the gameplay mod is told without its files, and the dialog warns
        out = self.run_room('w', 'gameplay-lobby')
        self.assertTrue(any('may desync' in t for _, t in out['lines']), out['lines'])
        self.assertTrue(any('cannot be had here' in t for _, t in out['lines']))

    def test_lobby_download_then_restart(self):
        self.host_mod('Pikachu Ice', {SKIN: b'ice' * 2000})
        out = self.run_room('wlaw', 'lobby')
        self.assertEqual(out['steps'][-1][0], AFTER)
        self.assertEqual(out['buttons'], ['Restart and rejoin', 'Later'])
        shutil.rmtree(self.dir / 'me' / 'mods_join')
        out = self.run_room('wlawa', 'lobby')   # (Restart and rejoin: chosen when it opens)
        self.assertEqual(out['steps'][-1][1], EV_RESTART)
        self.assertEqual((out['env'], out['keep']), ('0000000000001234:mirror', '1'))

    def test_lobby_later(self):
        self.host_mod('Pikachu Ice', {SKIN: b'ice' * 2000})
        out = self.run_room('wlawb', 'lobby')   # (Later)
        self.assertEqual(out['steps'][-1][:2], (IDLE, EV_PLAY))
        self.assertEqual(out['env'], '')
        self.assertIn('Kept for the next time', out['note'])
        out = self.run_room('w', 'lobby')   # (the next time: nothing to download - Restart and rejoin offered)
        self.assertEqual(out['steps'][-1][0], AFTER)
        self.assertIn('are here', out['title'])


if __name__ == '__main__':
    unittest.main()
