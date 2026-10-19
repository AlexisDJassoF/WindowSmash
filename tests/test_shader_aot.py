"""tools/shader_aot.py's scan of data.arc for GPU programs. Made-up containers; no game files."""
from pathlib import Path
import sys
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import shader_aot  # noqa: E402


class EmptyFilesTests(unittest.TestCase):
    def test_empty_files_are_not_unreadable(self):
        """data.arc has a few empty files marked compressed (the same 7 in 11.0.0 and 13.0.5): nothing
        to read in them. Prepare Shaders said "7 files could not be read" for them."""
        reads = []

        def read(off, n):
            reads.append(off)
            return b'\x28\xb5\x2f\xfd broken'[:n]   # (a zstd frame cut short)
        arc = types.SimpleNamespace(read=read)
        found = [('eff', (100, 0, 0, 3)), ('arc', (200, 0, 0, 3)), ('eff', (300, 8, 64, 3))]
        with mock.patch.object(shader_aot, 'containers', lambda a: found):
            programs, unread = shader_aot.find_programs(arc)
        self.assertEqual(unread, 1)   # (only the one whose data is broken)
        self.assertEqual(reads, [300])
        self.assertEqual(programs, [])


if __name__ == '__main__':
    unittest.main()
