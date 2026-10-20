"""tools/linux/package_linux.py on a Linux machine: the copied package, the --link one to play
here, and moving between the two. A made-up build and game tree; no compiler needed."""
from pathlib import Path
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'tools/linux/package_linux.py'


@unittest.skipUnless(sys.platform.startswith('linux'), 'symbolic links and a Linux layout')
class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='package-test-')
        self.addCleanup(self.temp.cleanup)
        top = Path(self.temp.name)
        self.toolchain, self.build, self.tree, self.out = top / 'tc', top / 'build-linux', top / 'tree', top / 'dist'
        libs = self.toolchain / 'dxvk-native/usr/lib'
        libs.mkdir(parents=True)
        for so in ('libdxvk_d3d11.so.0', 'libdxvk_dxgi.so.0'):
            (libs / so).write_bytes(b'so' * 100)
        self.build.mkdir()
        (self.build / 'ssbu').write_bytes(b'\x7fELF' + b'0' * 10000)
        (self.build / 'ssbu.syms').write_text('syms\n')
        (self.build / 'port.txt').write_text(f'{self.tree}\n')
        dump = self.tree / 'dump'
        for folder in ('modules', 'exefs', 'fonts', 'shadercache', 'nro'):
            (dump / folder).mkdir(parents=True)
        (dump / 'modules/main.reloc.bin').write_bytes(b'r' * 5000)
        (dump / 'modules/aoc.cfg').write_text('1\n')
        (dump / 'exefs/main').write_bytes(b'nso')
        (dump / 'fonts/nintendo_ext_003.bfttf').write_bytes(b'font')
        self.game = top / 'game data' / 'romfs.bin'
        self.game.parent.mkdir()
        self.game.write_bytes(b'romfs')
        (dump / 'modules/romfs.cfg').write_text(f'{self.game}\n0\n')
        (self.tree / 'save').mkdir()
        (self.tree / 'save/system.dat').write_bytes(b'save')
        self.bundle = self.out / 'ssbu-linux'

    def package(self, *args):
        env = {**os.environ, 'SSBU_LINUX_TOOLCHAIN': str(self.toolchain)}
        env.pop('SSBU_LLVM_BIN', None)
        return subprocess.run([sys.executable, str(SCRIPT), '--build', str(self.build), '--port', str(self.tree),
                               '--out', str(self.out), *args], capture_output=True, text=True, env=env)

    def test_link_and_tar_refused(self):
        r = self.package('--link', '--tar')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("don't go together", r.stdout + r.stderr)
        self.assertFalse(self.out.exists())

    def test_link(self):
        r = self.package('--link')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        for linked in ('ssbu', 'ssbu.syms', 'lib/libdxvk_d3d11.so.0', 'dump/modules/main.reloc.bin', 'dump/exefs/main',
                       'dump/fonts/nintendo_ext_003.bfttf', 'dump/shadercache'):
            self.assertTrue((self.bundle / linked).is_symlink(), linked)
        self.assertEqual(os.readlink(self.bundle / 'dump/shadercache'), str(self.tree / 'dump/shadercache'))
        for own in ('dump/modules/aoc.cfg', 'save/system.dat', 'run.sh'):
            self.assertFalse((self.bundle / own).is_symlink(), own)
            self.assertTrue((self.bundle / own).exists(), own)
        # a new build is played without packaging again
        (self.build / 'ssbu').write_bytes(b'\x7fELF new')
        self.assertEqual((self.bundle / 'ssbu').read_bytes(), b'\x7fELF new')

    def test_romfs_file_named_by_its_linux_folder(self):
        self.assertEqual(self.package('--link').returncode, 0)
        files_line = next(line for line in (self.bundle / 'scripts/common.sh').read_text().splitlines()
                          if 'romfs.bin|' in line)
        self.assertIn(f'romfs.bin|5|{self.game.parent}', files_line)

    def test_tar_after_link_has_no_links_and_keeps_tree(self):
        self.assertEqual(self.package('--link').returncode, 0)
        (self.tree / 'dump/shadercache/v1_new.bin').write_bytes(b'shader')
        r = self.package('--tar')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertFalse((self.bundle / 'ssbu').is_symlink())
        self.assertFalse((self.bundle / 'dump/shadercache').is_symlink())
        self.assertEqual((self.build / 'ssbu').read_bytes()[:4], b'\x7fELF')   # (not written through)
        self.assertTrue((self.tree / 'dump/shadercache/v1_new.bin').exists())
        with tarfile.open(self.out / 'ssbu-linux.tar') as tar:
            members = tar.getmembers()
        self.assertFalse([m.name for m in members if m.issym() or m.islnk()])
        ssbu = next(m for m in members if m.name == 'ssbu-linux/ssbu')
        self.assertEqual(ssbu.mode, 0o755)

    def test_link_after_copy_keeps_compiled_shaders(self):
        self.assertEqual(self.package().returncode, 0)
        (self.bundle / 'dump/shadercache').mkdir(exist_ok=True)   # (the game makes it when it compiles one)
        (self.bundle / 'dump/shadercache/v1_played.bin').write_bytes(b'played')
        self.assertEqual(self.package('--link').returncode, 0)
        self.assertTrue((self.bundle / 'dump/shadercache').is_symlink())
        self.assertEqual((self.tree / 'dump/shadercache/v1_played.bin').read_bytes(), b'played')


if __name__ == '__main__':
    unittest.main()
