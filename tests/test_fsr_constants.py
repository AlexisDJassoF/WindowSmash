"""FSR 1's constants (runtime/gpu_fsr.c) bit for bit against AMD's FsrEasuConOffset and FsrRcasCon
(third_party/fidelityfx-fsr1, the verbatim FSR 1.0.2 headers, compiled for the CPU with A_CPU):
tests/fsr_constants_test.c, built here with the host's C compiler, compares them for upscales
from 720p and 1080p to 768p-4K, a crop away from the corner, and sharpness 0.0 to 2.0."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
CC = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')


@unittest.skipUnless(CC, 'a C compiler')
class FsrConstantsTest(unittest.TestCase):
    def test_constants_match_amd(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'fsr_constants_test'
            # (no fused multiply-adds: both sides rounded operation by operation, as ffx_a.h's C is written)
            r = subprocess.run([CC, '-O2', '-ffp-contract=off', '-w', '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'third_party/fidelityfx-fsr1'),
                                str(ROOT / 'tests/fsr_constants_test.c'), str(ROOT / 'runtime/gpu_fsr.c'), '-lm', '-o', str(exe)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, 'compiling the test failed:\n' + r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(r.stdout.startswith('ok '), r.stdout)

    def test_license_kept(self):
        text = (ROOT / 'runtime/gpu_fsr.c').read_text()
        self.assertIn('Copyright (c) 2021 Advanced Micro Devices, Inc.', text)
        self.assertIn('Permission is hereby granted, free of charge', text)
        self.assertTrue((ROOT / 'third_party/fidelityfx-fsr1/LICENSE.txt').is_file())


if __name__ == '__main__':
    unittest.main()
