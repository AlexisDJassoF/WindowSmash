"""The render resolution scale in translated shaders (runtime/gpu_shader.c): a pixel program that
reads its window position (IPA 0x70 / 0x74) and loads texels (TLDS), made here (tests/maxwell_asm.py),
translated and compiled with vkd3d-shader by tools/shadertest.c: the HLSL scales the position by
params.rtscale and the texel coordinates through tsxy_, and compiles.

Needs Linux, the Linux toolchain and vkd3d-shader built (tools/linux/build_vkd3d.py linux)."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))
from maxwell_asm import assemble  # noqa: E402
from test_shadertest_linux import READY  # noqa: E402

PROGRAM = [('ipa', 0, 0x70), ('ipa', 1, 0x74),
           ('tlds', 4, 0, 1, 0x10, 2),    # int3(x, y, 0)
           ('tlds', 6, 0, 1, 0x14, 12),   # int3(x, y, lod)
           ('exit',)]


@unittest.skipUnless(READY, 'needs Linux, the Linux toolchain and vkd3d-shader built')
class ShaderScaleTest(unittest.TestCase):
    def test_position_and_texel_loads(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'shadertest'
            r = subprocess.run([sys.executable, str(ROOT / 'tools/linux/build_shadertest.py'), '--out', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            folder = Path(tmp) / 'programs'
            folder.mkdir()
            (folder / '5_scale.bin').write_bytes(assemble(PROGRAM))
            r = subprocess.run([str(exe), str(folder), '-v'], capture_output=True, text=True, timeout=300)
            out = r.stdout
            self.assertIn('1 shaders: 0 failed to compile', out)
            self.assertIn('i.pos.x * (1.0f + params.rtscale.z)', out)
            self.assertIn('i.pos.y * (1.0f + params.rtscale.w)', out)
            self.assertEqual(out.count('tex0.Load(int3(tsxy_(int2('), 1, out)
            self.assertEqual(out.count('tex1.Load(int3(tsxy_(int2('), 1, out)


if __name__ == '__main__':
    unittest.main()
