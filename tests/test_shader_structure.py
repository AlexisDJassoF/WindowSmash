"""Structured shader control flow (runtime/gpu_shader_cfg.c) against the dispatch loop, on the CPU:
tests/shader_structure_test.c, compiled here with the host's C compiler, runs each program's
blocks both ways for many sequences of condition outcomes and compares them.

  synthetic: the shapes of NVIDIA's code (if/else, nested SSY/SYNC, PBK/BRK loops, exits, jump
             tables, crossing branches) and the ones that must stay dispatch loops, with the reason
  mutation:  the comparison notices a wrong tree (an if's arms swapped)
  fuzz:      random programs are refused or run alike
  programs:  Maxwell code (tests/maxwell_asm.py) through the translator's own decoding (Linux, with
             the Linux toolchain's DXVK headers for runtime/linux/include/windows.h)
  corpus:    the programs SSBU_DUMP_SHADERS wrote, when SSBU_SHADER_CORPUS names their folder"""
from pathlib import Path
import os
import random
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))
from maxwell_asm import assemble  # noqa: E402

CC = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
TOOLCHAIN = Path(os.environ.get('SSBU_LINUX_TOOLCHAIN') or ROOT.parent / 'linux-toolchain')
DXVK_INCLUDE = TOOLCHAIN / 'dxvk-headers' / 'include' / 'native'


def build(out, translator=False):
    sources = [ROOT / 'tests/shader_structure_test.c', ROOT / 'runtime/gpu_shader_cfg.c']
    flags = ['-O2', '-I', str(ROOT / 'runtime')]
    if translator:
        sources.append(ROOT / 'runtime/gpu_shader.c')
        flags += ['-DWITH_TRANSLATOR', '-w', '-I', str(ROOT / 'runtime/linux/include'),
                  '-I', str(DXVK_INCLUDE / 'windows'), '-I', str(DXVK_INCLUDE / 'directx')]
    r = subprocess.run([CC, *flags, *map(str, sources), '-o', str(out)], capture_output=True, text=True)
    if r.returncode:
        raise AssertionError(f'compiling the test failed:\n{r.stdout}{r.stderr}')
    return out


def run(exe, *args):
    return subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=600)


def random_programs(folder, count):
    """Vertex ('1_') and pixel ('5_') programs: nested if/else, loops with breaks and continues, exits."""
    def body(r, depth, lines, labels, brk, cont):
        for _ in range(r.randint(1, 4)):
            what = r.randrange(10 if depth < 4 else 4)
            p = '%sP%d' % ('!' if r.random() < .3 else '', r.randrange(3))
            if what in (0, 1):
                lines.append(('iadd32i', r.randrange(8), r.randrange(8), r.randrange(100)))
            elif what == 2:
                lines.append(('isetp', r.randrange(3), r.randrange(8), r.randrange(8), r.choice(['lt', 'eq', 'ge', 'ne'])))
            elif what == 3:
                if brk and r.random() < .5:
                    lines.append(('brk', p))
                elif cont and r.random() < .5:
                    lines.append(('bra', cont, p))
                else:
                    lines.append(('exit', p))
            elif what in (4, 5, 6):
                a, b = 'L%d' % len(labels), 'L%d' % (len(labels) + 1)
                labels += [a, b]
                lines += [('ssy', b), ('bra', a, p)]
                body(r, depth + 1, lines, labels, None, None)
                lines += [('sync',), (a + ':',)]
                body(r, depth + 1, lines, labels, None, None)
                lines += [('sync',), (b + ':',)]
            elif what in (7, 8):
                h, e = 'L%d' % len(labels), 'L%d' % (len(labels) + 1)
                labels += [h, e]
                lines += [('pbk', e), (h + ':',), ('iadd32i', 7, 7, 1), ('isetp', 2, 7, 6, 'ge'), ('brk', 'P2')]
                body(r, depth + 1, lines, labels, 'brk', h)
                lines += [('bra', h), (e + ':',)]
            else:
                lines.append(('exit', p))
    for k in range(count):
        r = random.Random(k)
        lines = [('ald', 0, 0x80, 4), ('ald', 4, 0x90, 4), ('mov32i', 6, 5 + r.randrange(20))]
        body(r, 0, lines, [], None, None)
        lines += [('ast', 0, 0x70, 4), ('ast', 4, 0x80, 4), ('exit',)]
        header = bytearray(0x50)
        stage = '5' if r.random() < .3 else '1'
        if stage == '5':   # a pixel shader: color 0 from r0..r3
            lines = [line for line in lines if line[0] not in ('ald', 'ast')]
            header[0x48] = 0xF
        (Path(folder) / f'{stage}_{k:04x}.bin').write_bytes(assemble(lines, bytes(header)))


@unittest.skipUnless(CC, 'no C compiler')
class ShaderStructureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = build(Path(cls.tmp.name) / 'shader_structure_test')

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_synthetic(self):
        r = run(self.exe, 'synth')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('ok   diamond: structured', r.stdout)
        self.assertIn('ok   irreducible: irreducible', r.stdout)
        self.assertIn('0 failure(s)', r.stdout)

    def test_mutation_is_noticed(self):
        r = run(self.exe, 'mutate')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('MISMATCH', r.stdout)   # where the traces part: printed
        self.assertIn(', 0 not noticed', r.stdout)

    def test_fuzz(self):
        r = run(self.exe, 'fuzz', '300')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(' 0 failure(s)', r.stdout)


@unittest.skipUnless(CC and sys.platform.startswith('linux') and (DXVK_INCLUDE / 'windows').is_dir(),
                     'needs Linux and the Linux toolchain (DXVK headers)')
class TranslatorFlowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = build(Path(cls.tmp.name) / 'shader_structure_test', translator=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_assembled_programs(self):
        folder = Path(self.tmp.name) / 'programs'
        folder.mkdir()
        random_programs(folder, 150)
        r = run(self.exe, 'corpus', str(folder))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r'\d+ programs with branches: [1-9]\d* structured, traces identical in all but 0')

    @unittest.skipUnless(os.environ.get('SSBU_SHADER_CORPUS'), 'SSBU_SHADER_CORPUS names no folder of dumped programs')
    def test_corpus(self):
        r = run(self.exe, 'corpus', os.environ['SSBU_SHADER_CORPUS'])
        print(r.stdout)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
