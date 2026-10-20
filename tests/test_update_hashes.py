"""tools/update.py: an update whose hash tree (IVFC level 5) is partly kept from the original
release, as 13.0.4's is - those parts are read from the base once it is known. A made-up patch:
no game files needed."""
from pathlib import Path
import bisect
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import update  # noqa: E402

OWN, BASE = 1, 0   # (a patch piece's storage: the update's own data, or the original release's)


def patch_with(pieces, own, end):
    """A Patch reading `own` as its own data, mapped by pieces [(virtual, physical, storage)]."""
    p = object.__new__(update.Patch)
    p.ind, p.iv, p.vend = pieces, [v for v, _ph, _st in pieces], end
    p.storage = lambda off, n: own[off:off + n]
    p.blocks = end // 32
    p.hash_level = (0, end)
    try:
        p.hashes = p.vread(*p.hash_level)
    except LookupError:
        p.hashes = None
    return p


class HashTreeTests(unittest.TestCase):
    def test_whole_tree_in_the_update(self):
        p = patch_with([(0, 0, OWN)], bytes(range(64)), 64)
        self.assertEqual(p.block_hash(1), bytes(range(32, 64)))
        p.load_hashes(None, None)   # (nothing left to load)

    def test_part_kept_from_the_original(self):
        own = b'A' * 32
        base_file = b'.' * 100 + b'B' * 32   # the base's RomFS section starts at 100 in its file
        p = patch_with([(0, 0, OWN), (32, 0, BASE)], own, 64)
        self.assertIsNone(p.hashes)
        p.load_hashes(100, lambda off, n: base_file[off:off + n])
        self.assertEqual(p.block_hash(0), b'A' * 32)
        self.assertEqual(p.block_hash(1), b'B' * 32)

    def test_part_kept_but_base_not_the_original(self):
        p = patch_with([(0, 0, OWN), (32, 0, BASE)], b'A' * 32, 64)
        with self.assertRaisesRegex(SystemExit, 'original release'):
            p.load_hashes(None, lambda off, n: b'')


if __name__ == '__main__':
    unittest.main()
