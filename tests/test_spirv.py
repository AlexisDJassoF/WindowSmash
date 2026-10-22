"""The Vulkan renderer's SPIR-V (runtime/linux/gpu_spirv*.c), compiled here with the host's C
compiler and the Linux toolchain's SPIRV-Headers, every module checked by spirv-val (the
spirv-tools package) for Vulkan 1.3.

  builder: gpu_spirv_builder.c makes a valid module, types and constants once each
  corpus:  every program of the game (dump/aot/programs.pack, as tests/test_shader_corpus.py), as it
           comes and as a dispatch loop, through gpu_spirv.c: its ShaderInfo the HLSL translator's, its
           module valid
           (SSBU_CORPUS_LIMIT=<n>: the first n distinct programs only)"""
from pathlib import Path
import glob
import os
import shutil
import subprocess
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor

ROOT = Path(__file__).resolve().parent.parent
CC = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
SPIRV_VAL = shutil.which('spirv-val')
TOOLCHAIN = Path(os.environ.get('SSBU_LINUX_TOOLCHAIN') or ROOT.parent / 'linux-toolchain')
DXVK_INCLUDE = TOOLCHAIN / 'dxvk-headers' / 'include' / 'native'
PACK = Path(os.environ.get('SSBU_AOT_PACK') or ROOT / 'dump' / 'aot' / 'programs.pack')
LIMIT = os.environ.get('SSBU_CORPUS_LIMIT', '')
SPIRV_INCLUDE = next(iter(sorted(glob.glob(str(TOOLCHAIN / 'src' / 'SPIRV-Headers-*' / 'include')))), None)


def build(out, main, *sources, extra=()):
    flags = ['-O1', '-g', '-Wall', '-Wno-missing-braces', '-I', str(ROOT / 'runtime' / 'linux'), '-I', str(ROOT / 'runtime'),
             '-I', SPIRV_INCLUDE, *extra]
    r = subprocess.run([CC, *flags, str(main), *map(str, sources), '-o', str(out)], capture_output=True, text=True)
    if r.returncode:
        raise AssertionError(f'compiling {main.name} failed:\n{r.stdout}{r.stderr}')
    return out


def validate(spv):
    r = subprocess.run([SPIRV_VAL, '--target-env', 'vulkan1.3', str(spv)], capture_output=True, text=True)
    return r.returncode == 0, r.stdout + r.stderr


@unittest.skipUnless(CC and SPIRV_VAL and SPIRV_INCLUDE, 'needs a C compiler, spirv-val (spirv-tools) and the toolchain\'s SPIRV-Headers')
class Builder(unittest.TestCase):
    def test_module(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            exe = build(tmp / 'builder', ROOT / 'tests' / 'spirv_builder_test.c', ROOT / 'runtime' / 'linux' / 'gpu_spirv_builder.c')
            r = subprocess.run([str(exe), str(tmp / 'out.spv')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            ok, why = validate(tmp / 'out.spv')
            self.assertTrue(ok, why)


@unittest.skipUnless(CC and SPIRV_VAL and SPIRV_INCLUDE and PACK.is_file() and (DXVK_INCLUDE / 'windows').is_dir(),
                     'needs a C compiler, spirv-val, the toolchain and the program pack (dump/aot/programs.pack)')
class Corpus(unittest.TestCase):
    """Every translation of the pack through both translators; each module validated, then deleted
    (thousands of them would not fit in /tmp at once)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='spirv_corpus_'))
        spv = cls.tmp / 'spv'
        spv.mkdir()
        rt = ROOT / 'runtime'
        exe = build(cls.tmp / 'corpus', ROOT / 'tests' / 'shader_corpus.c', *sorted(rt.glob('gpu_shader*.c')),
                    rt / 'linux' / 'gpu_spirv.c', rt / 'linux' / 'gpu_spirv_builder.c',
                    extra=['-DWITH_SPIRV', '-w', '-I', str(rt / 'linux' / 'include'), '-I', str(DXVK_INCLUDE / 'windows'),
                           '-I', str(DXVK_INCLUDE / 'directx')])
        cls.lines, cls.invalid, pending = [], [], []

        def check(path):
            ok, why = validate(path)
            path.unlink()
            return None if ok else (path.name, why)

        proc = subprocess.Popen([str(exe), str(PACK), *([LIMIT] if LIMIT else []), '--spirv', str(spv)], stdout=subprocess.PIPE, text=True)
        with ThreadPoolExecutor(os.cpu_count() or 4) as pool:
            for line in proc.stdout:
                if not line.startswith('spirv '):
                    continue
                parts = line.rstrip('\n').split(' ', 2)
                cls.lines.append(parts)
                if parts[2] in ('same', 'differs'):
                    pending.append(pool.submit(check, spv / (parts[1] + '.spv')))
                    if len(pending) >= 256:   # (a bounded number of modules on disk)
                        cls.invalid += [r for r in (f.result() for f in pending[:128]) if r]
                        del pending[:128]
            cls.invalid += [r for r in (f.result() for f in pending) if r]
        if proc.wait():
            raise AssertionError(f'the corpus tool failed ({proc.returncode})')

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_shader_info(self):
        self.assertTrue(self.lines, 'no program')
        differ = [l[1] for l in self.lines if l[2] == 'differs']
        failed = [' '.join(l[1:]) for l in self.lines if l[2] not in ('same', 'differs') and not l[2].startswith('unsupported-by-both')]
        self.assertFalse(failed, f'{len(failed)} not translated; the first:\n' + '\n'.join(failed[:10]))
        self.assertFalse(differ, f'{len(differ)} of {len(self.lines)} programs note another ShaderInfo; the first: ' + ' '.join(differ[:10]))

    def test_valid(self):
        made = sum(1 for l in self.lines if l[2] in ('same', 'differs'))
        self.assertTrue(made, 'no module made')
        self.assertFalse(self.invalid, f'{len(self.invalid)} of {made} modules are not valid; the first:\n' +
                         '\n'.join(f'{name}: {why[:600]}' for name, why in self.invalid[:3]))


if __name__ == '__main__':
    unittest.main()
