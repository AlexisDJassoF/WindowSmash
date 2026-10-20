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


class ReadErrorTests(unittest.TestCase):
    """A Steam Deck tester's SD card failed a read (EIO) part-way through data.arc: Prepare Shaders
    stopped with a traceback. Now the file is tried again, else left out."""

    def arc(self, bad, fail_times=99):
        tries = {}

        def read(off, n):
            tries[off] = tries.get(off, 0) + 1
            if off in bad and tries[off] <= fail_times:
                raise OSError(5, 'Input/output error', '/media/card/base.romfs')
            return b''
        return types.SimpleNamespace(read=read), tries

    def scan(self, arc, offsets):
        found = [('eff', (off, 8, 8, 0)) for off in offsets]
        errors = []
        with mock.patch.object(shader_aot, 'containers', lambda a: found):
            programs, unread = shader_aot.find_programs(arc, errors, pause=0)
        return unread, errors

    def test_an_unreadable_file_is_left_out(self):
        arc, tries = self.arc({200})
        unread, errors = self.scan(arc, [100, 200, 300])
        self.assertEqual(unread, 1)
        self.assertEqual(tries, {100: 1, 200: shader_aot.READ_TRIES, 300: 1})
        self.assertEqual([e.filename for e in errors], ['/media/card/base.romfs'])
        text = shader_aot.read_error_text(errors, '/somewhere')
        self.assertIn('Input/output error, in /media/card/base.romfs', text)
        self.assertNotIn('copy WindowSmash made', text)

    def test_a_second_try_can_read_it(self):
        arc, tries = self.arc({200}, fail_times=1)
        unread, errors = self.scan(arc, [100, 200, 300])
        self.assertEqual((unread, errors), (0, []))
        self.assertEqual(tries[200], 2)

    def test_a_disk_that_stops_answering(self):
        offsets = list(range(0, 100 * 50, 100))
        arc, tries = self.arc(set(offsets[5:]))
        with self.assertRaises(shader_aot.DiskGone):
            self.scan(arc, offsets)
        self.assertEqual(len(tries), 5 + shader_aot.READ_GIVE_UP)   # (stopped there, not after all 50)

    def test_a_copy_of_its_own(self):
        tree = str(ROOT)
        e = OSError(5, 'Input/output error', str(ROOT / 'dump' / 'modules' / 'base.romfs'))
        self.assertIn('copy WindowSmash made', shader_aot.read_error_text([e], tree))


class RomfsImageReadErrorTests(unittest.TestCase):
    def test_the_error_names_its_file(self):
        import nca
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / 'base.bin'
            src.write_bytes(bytes(64))
            layers = Path(d) / 'layers.bin'
            nca.write_layers(str(layers), 64, [(0, 0, 0)])
            cfg = Path(d) / 'romfs.cfg'
            cfg.write_text(f'layered\n{layers}\n{src}\n')
            image = nca.RomfsImage(str(cfg))
            self.assertEqual(image.read(8, 8), bytes(8))
            broken = mock.Mock()
            broken.read.side_effect = OSError(5, 'Input/output error')
            real, image.sources[0] = image.sources[0], broken
            with self.assertRaises(OSError) as caught:
                image.read(8, 8)
            self.assertEqual((caught.exception.errno, caught.exception.filename), (5, str(src)))
            real.close()


if __name__ == '__main__':
    unittest.main()
