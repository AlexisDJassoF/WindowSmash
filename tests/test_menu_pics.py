"""The start-up menu's pictures (runtime/gpu_menu_pics.c) without the game: tests/menu_pics_test.c,
built here, asks for pictures by key - a mod's preview.png (stb_image), a BNTX of a mod's, files that
are not there or not pictures - and prints what it got."""
from pathlib import Path
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parent.parent
CC = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
TOOLCHAIN = Path(os.environ.get('SSBU_LINUX_TOOLCHAIN') or ROOT.parent / 'linux-toolchain')
DXVK_INCLUDE = TOOLCHAIN / 'dxvk-headers' / 'include' / 'native'


def png(w, h, rgba):
    """A w x h PNG of one colour"""
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b''.join(b'\0' + bytes(rgba) * w for _ in range(h))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw))
            + chunk(b'IEND', b''))


def bntx_bc7(w, h):
    """A minimal BNTX of a w x h BC7 texture (block height 1 GOB): what runtime/gpu_bntx.c reads"""
    blocks = bytes(((w + 3) // 4 * 16 + 63) // 64 * 64 * (((h + 3) // 4 + 7) // 8 * 8))
    d = bytearray(0x1000)
    d[0:4] = b'BNTX'
    d[0x20:0x24] = b'NX  '
    struct.pack_into('<IQ', d, 0x24, 1, 0x200)   # one texture, its BRTI offset at 0x200
    struct.pack_into('<Q', d, 0x200, 0x300)
    d[0x300:0x304] = b'BRTI'
    struct.pack_into('<I', d, 0x300 + 0x1C, 0x2001)
    struct.pack_into('<II', d, 0x300 + 0x24, w, h)
    struct.pack_into('<I', d, 0x300 + 0x34, 0)
    struct.pack_into('<Q', d, 0x300 + 0x70, 0x400)
    struct.pack_into('<Q', d, 0x400, 0x1000)
    return bytes(d) + blocks


@unittest.skipUnless(sys.platform.startswith('linux') and CC and (DXVK_INCLUDE / 'directx').is_dir(),
                     'Linux, a C compiler and the Linux toolchain\'s DXVK headers')
class MenuPicsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='menu-pics-test-')
        cls.exe = Path(cls.temp.name) / 'menu_pics_test'
        flags = ['-O1', '-w', '-D_GNU_SOURCE', '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime/linux'),
                 '-I', str(ROOT / 'runtime/linux/include'), '-I', str(DXVK_INCLUDE / 'windows'), '-I', str(DXVK_INCLUDE / 'directx')]
        r = subprocess.run([CC, *flags, str(ROOT / 'tests/menu_pics_test.c'), *[str(ROOT / 'runtime' / f) for f in
                            ('gpu_menu_pics.c', 'gpu_bntx.c', 'gpu_overlay_quads.c')], '-lm', '-pthread', '-o', str(cls.exe)],
                           capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(f'compiling the test failed:\n{r.stdout}{r.stderr}')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_pics(self, *keys):
        r = subprocess.run([str(self.exe), *keys], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        return dict(line.split(' ', 1) for line in r.stdout.splitlines()), r.stderr

    def test_preview_png_bntx_and_failures(self):
        d = Path(self.temp.name)
        (d / 'preview.png').write_bytes(png(40, 30, (10, 200, 30, 255)))
        (d / 'icon.bntx').write_bytes(bntx_bc7(64, 64))
        (d / 'bad.png').write_bytes(b'not a png at all')
        (d / 'bad.bntx').write_bytes(b'BNTX' + bytes(60))
        keys = [f'file:{d}/preview.png', f'file:{d}/icon.bntx', f'file:{d}/bad.png', f'file:{d}/bad.bntx', f'file:{d}/missing.png',
                'arc:ui/replace/chara/chara_2/chara_2_mario_00.bntx']
        got, log = self.run_pics(*keys)
        self.assertEqual(got[keys[0]], 'tex=1 40x30 failed=0 format=0 rgba=10,200,30,255')
        self.assertEqual(got[keys[1]], 'tex=1 64x64 failed=0 format=3')   # (OVERLAY_TEX_BC7)
        for k in keys[2:]:
            self.assertTrue(got[k].startswith('tex=0 0x0 failed=1'), (k, got[k]))
        self.assertIn('bad.png', log)   # ([mods] with why, once each)
        self.assertIn('bad.bntx', log)


if __name__ == '__main__':
    unittest.main()
