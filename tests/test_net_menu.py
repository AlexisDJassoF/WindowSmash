"""The start-up menu's Network page (runtime/net_menu.c) without a window: tests/net_menu_test.c, built
here with the host's C compiler, plays scripts of keys against it and prints the page, netplay.ini's
effect and the SSBU_NET_* variables it sets."""
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
class NetMenuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='net-menu-test-')
        cls.exe = Path(cls.temp.name) / 'net_menu_test'
        flags = ['-O1', '-w', '-D_GNU_SOURCE', '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime/linux/include'),
                 '-I', str(DXVK_INCLUDE / 'windows'), '-I', str(DXVK_INCLUDE / 'directx')]
        r = subprocess.run([CC, *flags, str(ROOT / 'tests/net_menu_test.c'), str(ROOT / 'runtime/net_menu.c'), '-o', str(cls.exe)],
                           capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(f'compiling the test failed:\n{r.stdout}{r.stderr}')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(prefix='net-menu-run-')
        self.addCleanup(self.dir.cleanup)
        self.ini = Path(self.dir.name) / 'netplay.ini'

    def run_menu(self, script, **env):
        base = {k: v for k, v in os.environ.items() if not k.startswith('SSBU_NET')}
        base.update(USER='tester', SSBU_NETPLAY_INI=str(self.ini))
        r = subprocess.run([str(self.exe), script], capture_output=True, text=True, cwd=self.dir.name, env={**base, **env}, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        return dict(kv.split('=', 1) for kv in r.stdout.rstrip('\n').split('|'))

    def type(self, text):
        return ''.join('k' + c for c in text)

    def test_defaults(self):
        out = self.run_menu('')
        self.assertEqual((out['row'], out['mode'], out['name'], out['room'], out['port']), ('Play', 'As launched', 'tester', 'smash', '39420'))
        self.assertEqual(out['launched'], 'Offline')
        self.assertEqual(self.run_menu('a')['plays'], '1')

    def test_name_from_launcher_and_online_name(self):
        self.assertEqual(self.run_menu('', SSBU_NET_NAME='Alpha')['name'], 'Alpha')
        (Path(self.dir.name) / 'online_name.txt').write_text('Bravo\n')
        self.assertEqual(self.run_menu('')['name'], 'Bravo')

    def test_modes_cycle(self):
        self.assertEqual(self.run_menu('dr')['mode'], 'Create a room (host)')   # (down from Play wraps to Connection)
        self.assertEqual(self.run_menu('drr')['mode'], 'Join a room')
        self.assertEqual(self.run_menu('dl')['mode'], 'Join a room')
        self.assertEqual(self.run_menu('drrr')['mode'], 'As launched')

    def test_host_rows(self):
        rows = [self.run_menu('dr' + 'd' * i)['row'] for i in range(1, 5)]
        self.assertEqual(rows, ['Your name', 'Room name', 'UDP port', 'Play'])   # (information rows skipped)

    def test_join_rows_and_no_play_without_address(self):
        rows = [self.run_menu('drr' + 'd' * i)['row'] for i in range(1, 5)]
        self.assertEqual(rows, ['Your name', 'Room name', 'Host address', 'Play'])
        out = self.run_menu('drrua')   # (up from Connection: Play) - nothing to join yet
        self.assertEqual((out['plays'], out['row']), ('0', 'Host address'))

    def test_typing(self):
        out = self.run_menu('dd' + 'e' * 10 + self.type('Kim Lee'))
        self.assertEqual((out['row'], out['name'], out['text']), ('Your name', 'Kim Lee', '1'))
        out = self.run_menu('drrddd' + self.type('10.0.0.2:39420 x'))   # (no spaces in an address)
        self.assertEqual(out['host'], '10.0.0.2:39420x')
        out = self.run_menu('drddd' + 'eeeee' + self.type('70000'))   # (a port up to 65535)
        self.assertEqual(out['port'], '7000')
        self.assertEqual(self.run_menu('drdddr')['port'], '39421')
        self.assertEqual(self.run_menu('')['text'], '0')
        self.assertEqual(self.run_menu('d' + self.type('x'))['mode'], 'As launched')   # (Connection takes no text)

    def test_found_room(self):
        out = self.run_menu('drrdddfr')   # (the Host address row, a room found, > picks it)
        self.assertEqual((out['host'], out['room']), ('192.168.1.9:39420', 'dojo'))
        out = self.run_menu('drrdddfrp0')
        self.assertEqual(out['tip'], 'Kim\'s room "dojo" on your network')
        self.assertEqual(out['lan'], '1 room found - < > on Host address')
        self.assertEqual(self.run_menu('drrdddfrda')['plays'], '1')

    def test_saved_and_recent(self):
        self.run_menu('drrddd' + self.type('10.0.0.2:39420') + 'u' + 'e' * 5 + self.type('club') + 'W')
        text = self.ini.read_text()
        self.assertIn('mode = join', text)
        self.assertIn('recent = 10.0.0.2:39420 club', text)
        out = self.run_menu('')
        self.assertEqual((out['mode'], out['host'], out['room'], out['recent']), ('Join a room', '10.0.0.2:39420', 'club', '1'))
        out = self.run_menu('dddd' + 'e' * 20 + 'r')   # (the address cleared, > offers the recent one)
        self.assertEqual((out['host'], out['room']), ('10.0.0.2:39420', 'club'))
        self.run_menu('dddd' + 'e' * 20 + self.type('10.0.0.3') + 'W')
        self.assertEqual(self.run_menu('')['recent'], '2')
        self.assertLess(self.ini.read_text().index('10.0.0.3'), self.ini.read_text().index('10.0.0.2'))   # (newest first)

    def test_launcher_choice_over_saved_mode(self):
        self.run_menu('drW')   # (hosting saved)
        self.assertEqual(self.run_menu('')['mode'], 'Create a room (host)')
        self.assertEqual(self.run_menu('', SSBU_NET_MODE='launcher')['mode'], 'As launched')   # (join-netplay.sh...)
        self.run_menu('W', SSBU_NET_MODE='launcher')   # (played as launched: saved untouched... by gpu_menu.c, file_mode)
        out = self.run_menu('A')
        self.assertEqual(out['MENU_MODE'], 'host')
        self.assertEqual(self.run_menu('', SSBU_NET_MENU_MODE='join')['mode'], 'Join a room')   # (F8: the mode in use)

    def test_apply_host_and_join(self):
        out = self.run_menu('drA', SSBU_NET_RELAY='lobby.example:1')
        self.assertEqual((out['HOST'], out['RELAY'], out['PORT'], out['ROOM'], out['NAME']), ('1', '(unset)', '39420', 'smash', 'tester'))
        out = self.run_menu('drrddd' + self.type('10.0.0.2') + 'A', SSBU_NET_LAN='1')
        self.assertEqual((out['HOST'], out['RELAY'], out['LAN']), ('(unset)', '10.0.0.2', '(unset)'))

    def test_as_launched_restores_the_launcher(self):
        env = dict(SSBU_NET_RELAY='smash.recomps.wtf:39420', SSBU_NET_ROOM='ourcode')
        out = self.run_menu('SdrAlA', **env)   # kept aside, hosted, then back to As launched
        self.assertEqual((out['mode'], out['HOST'], out['RELAY'], out['ROOM']), ('As launched', '(unset)', 'smash.recomps.wtf:39420', 'ourcode'))
        self.assertEqual(out['launched'], 'Online: smash.recomps.wtf:39420, code ourcode')
        self.assertEqual(self.run_menu('', SSBU_NET_LAN='1')['launched'], 'LAN mode (with a Switch)')
        self.assertEqual(self.run_menu('', SSBU_NET_HOST='1', SSBU_NET_ROOM='x')['launched'], 'Hosting room x')
        # started again (F8): the launcher's variables as kept, not as the last mode left them
        out = self.run_menu('A', SSBU_NET_LAUNCH_SAVED='1', SSBU_NET_LAUNCH_RELAY='=lobby:5', SSBU_NET_HOST='1')
        self.assertEqual((out['launched'], out['HOST'], out['RELAY']), ('Online: lobby:5', '(unset)', 'lobby:5'))

    def test_router_and_tips(self):
        out = self.run_menu('drp1')
        self.assertEqual(out['addrs'], '192.168.1.5:39420')
        self.assertEqual(out['router'], 'UPnP works - internet: 203.0.113.7:39420')
        self.assertIn('203.0.113.7:39420', out['tip'])
        out = self.run_menu('drp4', TEST_WAN='100.70.1.2')
        self.assertEqual(out['router'], 'behind another NAT / CGNAT (100.70.1.2)')
        self.assertIn('VPN', out['tip'])
        out = self.run_menu('drp2', TEST_VPN='100.101.1.1')
        self.assertEqual(out['addrs'], '192.168.1.5:39420   VPN 100.101.1.1:39420')
        self.assertIn('100.101.1.1:39420', out['tip'])
        self.assertEqual(self.run_menu('drp0')['router'], 'checking the router (UPnP)...')
        self.assertIn('Type the host', self.run_menu('drrp0')['tip'])


if __name__ == '__main__':
    unittest.main()
