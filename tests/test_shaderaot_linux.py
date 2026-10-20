"""tools/shaderaot.c on Linux (tools/linux/build_shaderaot.py): a pack of Maxwell programs made here
(tests/maxwell_asm.py) compiled into a cache folder as the Linux renderer would - its file names,
dated as not yet used, written whole - with --dry's keys, --keys, --progress and the work shared
between processes.

Needs Linux, the Linux toolchain and vkd3d-shader built (tools/linux/build_vkd3d.py linux)."""
from pathlib import Path
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))
from maxwell_asm import assemble  # noqa: E402
from test_shadertest_linux import PROGRAMS, READY  # noqa: E402

VERSION = int(re.search(r'#define SHADER_CACHE_VERSION (\d+)', (ROOT / 'runtime/gpu_shader_cache.h').read_text()).group(1))
PLAYED_AFTER = 1009843200   # 2002-01-01 UTC (gpu_shader_cache.h)


def write_pack(path, programs):
    """tools/shader_aot.py's pack: vertex programs only (stage 0), unpaired."""
    with open(path, 'wb') as f:
        f.write(b'SAOT' + struct.pack('<I', len(programs)))
        for blob in programs:
            f.write(struct.pack('<3I', 0, len(blob), 0) + blob)


@unittest.skipUnless(READY, 'needs Linux, the Linux toolchain and vkd3d-shader built')
class ShaderaotLinuxTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        top = Path(cls.tmp.name)
        cls.exe = top / 'shaderaot'
        r = subprocess.run([sys.executable, str(ROOT / 'tools/linux/build_shaderaot.py'), '--out', str(cls.exe)],
                           capture_output=True, text=True)
        if r.returncode:
            raise AssertionError('building shaderaot failed:\n' + r.stdout + r.stderr)
        cls.pack = top / 'programs.pack'
        write_pack(cls.pack, [assemble(PROGRAMS['diamond']), assemble(PROGRAMS['loop']), assemble(PROGRAMS['irreducible'])])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def aot(self, cache, *args):
        r = subprocess.run([str(self.exe), str(self.pack), str(cache), *args], capture_output=True, text=True, timeout=600)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r

    def keys(self, cache):
        return [line.split() for line in self.aot(cache, '0', '1', '--dry').stdout.splitlines()]

    def test_compiles_as_the_renderer_names_them(self):
        with tempfile.TemporaryDirectory() as cache:
            keys = self.keys(cache)
            self.assertEqual(len(keys), 3)
            r = self.aot(cache, '0', '1')
            self.assertIn('3 compiled', r.stderr)
            files = sorted(os.listdir(cache))
            self.assertEqual(len(files), 3)
            for f in files:
                self.assertRegex(f, rf'^v{VERSION}_vs_[0-9a-f]{{16}}_00000000_0{{16}}\.dxbc$')
                self.assertLess(os.path.getmtime(os.path.join(cache, f)), PLAYED_AFTER)   # not yet used
                self.assertEqual(open(os.path.join(cache, f), 'rb').read(4), b'DXBC')
            again = self.aot(cache, '0', '1')   # what is there is left as it is
            self.assertIn('3 already cached', again.stderr)

    def test_keys_and_progress(self):
        with tempfile.TemporaryDirectory() as cache:
            first = self.keys(cache)[0]
            listed = Path(self.tmp.name) / 'keys.txt'   # (not in the cache folder)
            listed.write_text(' '.join(first) + '\n')
            r = self.aot(cache, '0', '1', '--keys', str(listed), '--progress', '--die-with-parent')
            self.assertEqual(r.stdout.splitlines(), ['done 1 0 0'])
            self.assertEqual(os.listdir(cache), [f'v{VERSION}_vs_{first[1]}_00000000_0000000000000000.dxbc'])

    def test_parts_share_the_work(self):
        with tempfile.TemporaryDirectory() as cache:
            compiled = 0
            for part in range(2):
                last = self.aot(cache, str(part), '2', '--progress').stdout.splitlines()[-1]
                compiled += int(last.split()[1])   # (each part's own count: "done <compiled> <cached> <left out>")
            self.assertEqual(compiled, 3)
            self.assertEqual(len(os.listdir(cache)), 3)
            self.assertFalse([f for f in os.listdir(cache) if f.endswith('.tmp')])


if __name__ == '__main__':
    unittest.main()
