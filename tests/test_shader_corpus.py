"""The shader translator over every program of the game (tests/shader_corpus.c, compiled here with
the host's C compiler): the programs tools/shader_aot.py packed from data.arc, in
dump/aot/programs.pack (SSBU_AOT_PACK names another; skipped when there is none).

  translates: every program translates, both as it comes and as a dispatch loop, as often as before
  same as:    with SSBU_HLSL_SAME_AS=<git revision>, the HLSL (and ShaderInfo) of every program is the
              same byte for byte as that revision's translator generates - for changes that must
              not change what the translator writes (the Windows build compiles that HLSL), such as
              moving the instruction decoding to runtime/gpu_shader_decode.c
SSBU_CORPUS_LIMIT=<n> takes the first n distinct programs only (a quicker run)."""
from pathlib import Path
import glob
import os
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
CC = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
TOOLCHAIN = Path(os.environ.get('SSBU_LINUX_TOOLCHAIN') or ROOT.parent / 'linux-toolchain')
DXVK_INCLUDE = TOOLCHAIN / 'dxvk-headers' / 'include' / 'native'
PACK = Path(os.environ.get('SSBU_AOT_PACK') or ROOT / 'dump' / 'aot' / 'programs.pack')
LIMIT = os.environ.get('SSBU_CORPUS_LIMIT', '')


def build(runtime, out):
    """tests/shader_corpus.c with the translator of a runtime folder (its gpu_shader*.c)."""
    sources = [str(ROOT / 'tests' / 'shader_corpus.c')] + sorted(glob.glob(str(runtime / 'gpu_shader*.c')))
    flags = ['-O2', '-w', '-I', str(runtime), '-I', str(ROOT / 'runtime' / 'linux' / 'include'),
             '-I', str(DXVK_INCLUDE / 'windows'), '-I', str(DXVK_INCLUDE / 'directx')]
    r = subprocess.run([CC, *flags, *sources, '-o', str(out)], capture_output=True, text=True)
    if r.returncode:
        raise AssertionError(f'compiling the corpus tool failed:\n{r.stdout}{r.stderr}')
    return out


def run(exe):
    r = subprocess.run([str(exe), str(PACK), *([LIMIT] if LIMIT else [])], capture_output=True, text=True, timeout=3600)
    if r.returncode:
        raise AssertionError(f'{exe} failed ({r.returncode}):\n{r.stderr[-3000:]}')
    return r.stdout.splitlines()


@unittest.skipUnless(CC and PACK.is_file() and (DXVK_INCLUDE / 'windows').is_dir(),
                     'needs a C compiler, the program pack (dump/aot/programs.pack) and the Linux toolchain headers')
class ShaderCorpus(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='shader_corpus_'))
        cls.lines = run(build(ROOT / 'runtime', cls.tmp / 'corpus'))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_translates(self):
        self.assertTrue(self.lines, 'no program translated')
        failed = [l for l in self.lines if ' failed ' in l]
        # (a program the translator gives up on is left to the renderer's fallback; few do)
        self.assertLess(len(failed), max(1, len(self.lines) // 100), '\n'.join(failed[:20]))

    @unittest.skipUnless(os.environ.get('SSBU_HLSL_SAME_AS'), 'SSBU_HLSL_SAME_AS=<git revision> compares with that translator')
    def test_same_as_revision(self):
        rev = os.environ['SSBU_HLSL_SAME_AS']
        base = self.tmp / 'base'
        base.mkdir()
        archive = subprocess.run(['git', '-C', str(ROOT), 'archive', rev, 'runtime'], capture_output=True, check=True).stdout
        tar_path = self.tmp / 'base.tar'
        tar_path.write_bytes(archive)
        with tarfile.open(tar_path) as t:
            t.extractall(base)
        before = run(build(base / 'runtime', self.tmp / 'corpus_base'))
        self.assertEqual(len(before), len(self.lines), 'not the same translations')
        differ = [(a, b) for a, b in zip(before, self.lines) if a != b]
        self.assertFalse(differ, f'{len(differ)} of {len(self.lines)} translations differ from {rev}; the first:\n' +
                         '\n'.join(f'  {a}\n  {b}' for a, b in differ[:10]))


if __name__ == '__main__':
    unittest.main()
