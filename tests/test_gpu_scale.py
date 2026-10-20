"""The render resolution scale's arithmetic (runtime/gpu_scale.c): which render targets are scaled,
their sizes and scissors, and the shaders' coordinate fix-ups - unchanged values when unscaled -
through tests/gpu_scale_test.c, built here with the host's C compiler; and that the translator
(runtime/gpu_shader.c) emits those same fix-ups as HLSL."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
CC = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')


@unittest.skipUnless(CC, 'a C compiler')
class ScaleTests(unittest.TestCase):
    def test_arithmetic(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'gpu_scale_test'
            r = subprocess.run([CC, '-O2', '-ffp-contract=off', '-I', str(ROOT / 'runtime'), str(ROOT / 'tests/gpu_scale_test.c'),
                                str(ROOT / 'runtime/gpu_scale.c'), '-lm', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.stdout.strip(), 'ok', r.stdout)

    def test_translator_emits_the_same(self):
        src = (ROOT / 'runtime/gpu_shader.c').read_text()
        self.assertIn('return d == 0.0f ? p : int2(floor((float2(p) + 0.5f) * (1.0f + d)));', src)
        self.assertIn('(i.pos.x * (1.0f + params.rtscale.z))', src)
        self.assertIn('i.pos.y * (1.0f + params.rtscale.w)', src)
        self.assertIn('float4 rtscale; float4 texscale[16];', src)


if __name__ == '__main__':
    unittest.main()
