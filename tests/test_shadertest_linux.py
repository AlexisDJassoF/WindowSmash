"""tools/shadertest.c on Linux (tools/linux/build_shadertest.py): the translator's HLSL compiled with
vkd3d-shader, as the Linux renderer compiles it, for Maxwell programs made here (tests/maxwell_asm.py):
structured ones compile, nothing compiles only as a dispatch loop, and the dispatch loop stays where
the graph is irreducible.

Needs Linux, the Linux toolchain and vkd3d-shader built (tools/linux/build_vkd3d.py linux)."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))
from maxwell_asm import assemble  # noqa: E402
from test_shader_structure import random_programs  # noqa: E402

TOOLCHAIN = Path(os.environ.get('SSBU_LINUX_TOOLCHAIN') or ROOT.parent / 'linux-toolchain')
READY = sys.platform.startswith('linux') and (TOOLCHAIN / 'build-vkd3d/linux/libvkd3d-shader.a').is_file() \
    and (TOOLCHAIN / 'sysroot').is_dir()

PROGRAMS = {
    'diamond': [('ald', 0, 0x80, 4), ('isetp', 0, 0, 1, 'lt'), ('ssy', 'L'), ('bra', 'A', 'P0'), ('iadd32i', 2, 0, 1), ('sync',),
                ('A:',), ('iadd32i', 2, 1, 5), ('sync',), ('L:',), ('ast', 0, 0x70, 4), ('ast', 2, 0x80, 1), ('exit',)],
    'loop': [('ald', 0, 0x80, 4), ('mov32i', 4, 0), ('pbk', 'E'), ('H:',), ('iadd32i', 4, 4, 1), ('isetp', 1, 4, 0, 'ge'), ('brk', 'P1'),
             ('iadd32i', 2, 2, 3), ('bra', 'H'), ('E:',), ('ast', 0, 0x70, 4), ('ast', 2, 0x80, 1), ('exit',)],
    'irreducible': [('ald', 0, 0x80, 4), ('isetp', 0, 0, 1, 'lt'), ('bra', 'B', 'P0'), ('A:',), ('iadd32i', 2, 2, 1), ('B:',),
                    ('iadd32i', 2, 2, 2), ('isetp', 1, 2, 3, 'lt'), ('bra', 'A', 'P1'), ('ast', 0, 0x70, 4), ('ast', 2, 0x80, 1), ('exit',)],
}


@unittest.skipUnless(READY, 'needs Linux, the Linux toolchain and vkd3d-shader built')
class ShadertestLinuxTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name) / 'shadertest'
        r = subprocess.run([sys.executable, str(ROOT / 'tools/linux/build_shadertest.py'), '--out', str(cls.exe)],
                           capture_output=True, text=True)
        if r.returncode:
            raise AssertionError('building shadertest failed:\n' + r.stdout + r.stderr)
        cls.folder = Path(cls.tmp.name) / 'programs'
        cls.folder.mkdir()
        for name, lines in PROGRAMS.items():
            (cls.folder / f'1_{name}.bin').write_bytes(assemble(lines))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def shadertest(self, folder, *args):
        r = subprocess.run([str(self.exe), str(folder), *args], capture_output=True, text=True, timeout=600)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout

    def test_control_flow(self):
        out = self.shadertest(self.folder, '-v')
        self.assertIn('3 shaders: 0 failed to compile', out)
        self.assertIn('control flow: 0 linear, 2 structured, 1 dispatch loops (irreducible 1)', out)
        diamond = out[out.index('==== 1_diamond.bin'):out.index('1_diamond.bin: ')]
        self.assertNotIn('switch (pc_)', diamond)
        self.assertIn('} else {', diamond)
        loop = out[out.index('==== 1_loop.bin'):out.index('1_loop.bin: ')]
        self.assertNotIn('switch (pc_)', loop)
        main = loop[loop.index('VSOut main('):]
        self.assertEqual(main.count('[loop]'), 1)
        self.assertNotRegex(main, r'\bbreak;')   # (fxc mishandles break in an if: a run flag instead)
        self.assertIn('switch (pc_)', out[out.index('==== 1_irreducible.bin'):])

    def test_random_programs(self):
        folder = Path(self.tmp.name) / 'random'
        folder.mkdir()
        random_programs(folder, 120)
        out = self.shadertest(folder, '-q')
        self.assertIn('120 shaders: 0 failed to compile', out)
        self.assertIn('; 0 that compile only as dispatch loops', out)
        now, before = map(int, re.search(r'DXBC instructions: (\d+) as translated, (\d+) with every', out).groups())
        self.assertLess(now, before)


if __name__ == '__main__':
    unittest.main()
