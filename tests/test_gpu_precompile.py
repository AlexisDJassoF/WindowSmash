"""The start-up's shader screen (runtime/gpu_precompile.c) without the game or a GPU:
tests/gpu_precompile_test.c, built here with the host's C compiler, drives the keys-against-cache
difference, shaderaot's progress lines and the whole run, with a stand-in for shaderaot that
writes empty cache files and prints what the real one does."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
CC = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
TOOLCHAIN = Path(os.environ.get('SSBU_LINUX_TOOLCHAIN') or ROOT.parent / 'linux-toolchain')
DXVK_INCLUDE = TOOLCHAIN / 'dxvk-headers' / 'include' / 'native'
VERSION = int(re.search(r'#define SHADER_CACHE_VERSION (\d+)', (ROOT / 'runtime/gpu_shader_cache.h').read_text()).group(1))

# shaderaot's command line, its --dry list, --keys and --progress, writing empty files
FAKE_AOT = r'''#!/usr/bin/env python3
import os, sys, time
pack, cache, part, parts = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
args = sys.argv[5:]
if "--dry" in args:
    for i in range(int(os.environ.get("FAKE_KEYS", "6"))):
        print(f"vs {i:016x}" if i % 2 == 0 else f"ps {i:016x} 0000000f")
    sys.exit(0)
keys = [l.split() for l in open(args[args.index("--keys") + 1]) if l.strip()]
done = 0
for i, k in enumerate(keys):
    if i % parts != part:
        continue
    name = (f"v{VERSION}_vs_{k[1]}_00000000_0000000000000000.dxbc" if k[0] == "vs"
            else f"v{VERSION}_ps_{k[1]}_{k[2]}_0000000000000000.dxbc")
    time.sleep(float(os.environ.get("FAKE_DELAY", "0")))
    open(os.path.join(cache, name), "wb").close()
    done += 1
    print(f"done {done} 0 0", flush=True)
'''.replace('{VERSION}', str(VERSION), 1).replace('v{VERSION}', f'v{VERSION}')


def vs_name(h):
    return f'v{VERSION}_vs_{h:016x}_00000000_0000000000000000.dxbc'


@unittest.skipUnless(sys.platform.startswith('linux') and CC and (DXVK_INCLUDE / 'directx').is_dir(),
                     'Linux, a C compiler and the Linux toolchain\'s DXVK headers')
class PrecompileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='precompile-test-')
        cls.exe = Path(cls.temp.name) / 'gpu_precompile_test'
        flags = ['-O1', '-w', '-D_GNU_SOURCE', '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime/linux/include'),
                 '-I', str(DXVK_INCLUDE / 'windows'), '-I', str(DXVK_INCLUDE / 'directx')]
        r = subprocess.run([CC, *flags, str(ROOT / 'tests/gpu_precompile_test.c'), str(ROOT / 'runtime/gpu_precompile.c'),
                            '-o', str(cls.exe)], capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(f'compiling the test failed:\n{r.stdout}{r.stderr}')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix='tree-', dir=self.temp.name))
        self.modules, self.aot, self.cache = self.dir / 'dump/modules', self.dir / 'dump/aot', self.dir / 'dump/shadercache'
        for d in (self.modules, self.aot, self.cache):
            d.mkdir(parents=True)
        self.fake = self.dir / 'shaderaot'
        self.fake.write_text(FAKE_AOT)
        self.fake.chmod(0o755)

    def run_exe(self, *args, **env):
        e = {**os.environ, 'SSBU_SHADERAOT': str(self.fake), **env}
        e.pop('SSBU_SHADER_CACHE', None)
        return subprocess.run([str(self.exe), *args], capture_output=True, text=True, env=e, timeout=120)

    def write_keys(self, n):
        keys = self.aot / f'keys_v{VERSION}.txt'
        keys.write_text(''.join(f'vs {i:016x}\n' for i in range(n)))
        return keys

    def missing(self, keys):
        out = self.run_exe('missing', str(keys), str(self.cache)).stdout.split()
        return int(out[-1]), out

    def test_missing(self):
        keys = self.aot / 'keys.txt'
        keys.write_text('vs 0000000000000001\nps 0000000000000002 0000003f\nvs 0000000000000003\nnot a key\n')
        (self.cache / vs_name(1)).touch()
        (self.cache / (vs_name(3) + '.1234.tmp')).touch()                                  # half written: missing
        (self.cache / f'v{VERSION - 1}_ps_0000000000000002_0000003f_0000000000000000.dxbc').touch()  # an older version: missing
        n, out = self.missing(keys)
        self.assertEqual(n, 2)
        self.assertIn('0000000000000003', out)
        self.assertNotIn('0000000000000001', out)

    def test_failed_marker_counts_as_tried(self):   # (shaderaot could not compile it: not every start again)
        keys = self.aot / 'keys.txt'
        keys.write_text('vs 0000000000000001\nvs 0000000000000002\n')
        (self.cache / (vs_name(1) + '.failed')).touch()
        (self.cache / (f'v{VERSION - 1}_vs_0000000000000002_00000000_0000000000000000.dxbc.failed')).touch()   # (an older version's: tried again)
        n, out = self.missing(keys)
        self.assertEqual(n, 1)
        self.assertIn('0000000000000002', out)

    def test_missing_without_keys(self):
        self.assertEqual(self.missing(self.aot / 'none.txt')[0], -1)

    def test_missing_is_fast(self):
        keys = self.write_keys(100000)
        for i in range(0, 100000, 2):
            (self.cache / vs_name(i)).touch()
        t = time.monotonic()
        n, _ = self.missing(keys)
        self.assertEqual(n, 50000)
        self.assertLess(time.monotonic() - t, 1.0)

    def test_done_line(self):
        self.assertEqual(self.run_exe('done', 'done 3 4 5').stdout.split(), ['3', '4', '5'])
        self.assertEqual(self.run_exe('done', 'part 0/1: 3 keys').stdout.split(), ['no'])

    def test_no_index(self):
        r = self.run_exe('run', str(self.modules))
        self.assertIn('no shader index', r.stderr)

    def test_run_compiles_what_is_missing(self):
        (self.aot / 'programs.pack').write_bytes(b'SAOT')
        self.write_keys(40)
        for i in range(10):
            (self.cache / vs_name(i)).touch()
        r = self.run_exe('run', str(self.modules))
        self.assertIn('30 shaders missing', r.stderr)
        self.assertIn('precompiled 30 of 30 (0 failed or left out, 0 left)', r.stderr)
        self.assertIn('screen: Preparing shaders', r.stdout)
        self.assertEqual(len(list(self.cache.iterdir())), 40)
        self.assertFalse((self.aot / 'missing.txt').exists())
        again = self.run_exe('run', str(self.modules))   # nothing left: no screen
        self.assertNotIn('screen:', again.stdout)
        self.assertNotIn('missing', again.stderr)

    def test_skip_and_resume(self):
        (self.aot / 'programs.pack').write_bytes(b'SAOT')
        self.write_keys(200)
        r = self.run_exe('run', str(self.modules), FAKE_DELAY='0.02', TEST_SKIP_AT='20')
        self.assertIn('Esc pressed', r.stderr)
        made = len(list(self.cache.iterdir()))
        self.assertLess(made, 200)
        self.assertFalse(any(p.suffix == '.tmp' for p in self.cache.iterdir()))
        again = self.run_exe('run', str(self.modules))
        self.assertIn(f'{200 - made} shaders missing', again.stderr)
        self.assertEqual(len(list(self.cache.iterdir())), 200)

    def test_keys_made_again_for_a_new_version(self):
        (self.aot / 'programs.pack').write_bytes(b'SAOT')
        (self.aot / f'keys_v{VERSION - 1}.txt').write_text('vs 0000000000000001\n')
        r = self.run_exe('run', str(self.modules), FAKE_KEYS='6')
        self.assertIn('indexing', r.stderr)
        self.assertTrue((self.aot / f'keys_v{VERSION}.txt').exists())
        self.assertIn('precompiled 6 of 6', r.stderr)

    def test_turned_off(self):
        (self.aot / 'programs.pack').write_bytes(b'SAOT')
        self.write_keys(5)
        r = self.run_exe('run', str(self.modules), SSBU_PRECOMPILE='0')
        self.assertEqual(r.stderr, '')
        self.assertEqual(len(list(self.cache.iterdir())), 0)


if __name__ == '__main__':
    unittest.main()
