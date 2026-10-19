"""tools/nca.py's packages: the NCAs inside an .nsp (PFS0) and an .xci (HFS0), with made-up files."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import nca  # noqa: E402


def partition(files, hfs):
    """A PFS0 (0x18-byte entries) or HFS0 (0x40-byte entries) of (name, data)."""
    strings, entries, data = b'', b'', b''
    for name, body in files:
        entry = struct.pack('<QQI', len(data), len(body), len(strings))
        entries += entry + bytes((0x40 if hfs else 0x18) - len(entry))
        strings += name.encode() + b'\0'
        data += body
    head = (b'HFS0' if hfs else b'PFS0') + struct.pack('<II', len(files), len(strings)) + bytes(4)
    return head + entries + strings + data


def xci(secure_files, prefix=0x200):
    """A game card image: its header ("HEAD" at 0x100, the root partition's place at 0x130), then
    the root HFS0 with the update, normal and secure partitions."""
    root = partition([('update', partition([], True)), ('normal', partition([], True)),
                      ('secure', partition(secure_files, True))], True)
    head = bytearray(prefix)
    head[0x100:0x104] = b'HEAD'
    struct.pack_into('<Q', head, 0x130, prefix)
    return bytes(head) + root


class ContainerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nca-containers-')
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)

    def write(self, name, data):
        path = self.dir / name
        path.write_bytes(data)
        return str(path)

    def read(self, path, off, size):
        with open(path, 'rb') as f:
            f.seek(off)
            return f.read(size)

    def test_xci_secure_partition(self):
        path = self.write('game [v0].xci', xci([('aa.nca', b'PROGRAM-NCA'), ('bb.cnmt.nca', b'META'), ('cert', b'x')]))
        self.assertEqual(nca.container_kind(path), 'xci')
        files = nca.xci_files(path)
        self.assertEqual([n for n, _o, _s in files], ['aa.nca', 'bb.cnmt.nca', 'cert'])
        name, off, size = files[0]
        self.assertEqual(self.read(path, off, size), b'PROGRAM-NCA')
        self.assertEqual([n for n, _o, _s in nca.container_files(path)], ['aa.nca', 'bb.cnmt.nca'])   # NCAs only
        self.assertEqual(nca.nsp_title_keys(path), {})   # a game card's NCAs need no tickets

    def test_nsp(self):
        path = self.write('update.nsp', partition([('cc.nca', b'UPDATE-PROGRAM'), ('dd.tik', b'')], False))
        self.assertEqual(nca.container_kind(path), 'nsp')
        files = nca.container_files(path)
        self.assertEqual([n for n, _o, _s in files], ['cc.nca'])
        self.assertEqual(self.read(path, files[0][1], files[0][2]), b'UPDATE-PROGRAM')

    def test_bare_nca_and_bad_xci(self):
        path = self.write('program.nca', bytes(0x400))
        self.assertEqual(nca.container_kind(path), 'nca')
        self.assertEqual(nca.container_files(path), [('program.nca', 0, 0x400)])
        no_secure = bytearray(xci([]))
        bad = self.write('bad.xci', bytes(no_secure).replace(b'secure', b'normaL'))
        with self.assertRaises(SystemExit):
            nca.xci_files(bad)


if __name__ == '__main__':
    unittest.main()
