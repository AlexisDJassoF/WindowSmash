"""An update that reuses the original release's hashes for blocks the same as in that release:
update.py opens it and checks those blocks through the hash tree's levels above. Made-up hash
trees with 64-byte blocks (the game's are 16 KB); no game files."""
from pathlib import Path
import hashlib
import struct
import sys
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import update  # noqa: E402

BLOG = 6
BS = 1 << BLOG   # two hashes per block, so every level of a small tree is used
BLOCKS = 16


def sha(data):
    return hashlib.sha256(data).digest()


def ivfc(l6):
    """Levels 1 to 6 (each padded with zeros to whole blocks) and the master hash: each hash
    level holds the SHA-256 of every block of the level below."""
    levels = [l6]
    for _ in range(5):
        below = levels[0]
        h = b''.join(sha(below[j:j + BS]) for j in range(0, len(below), BS))
        levels.insert(0, h + bytes(-len(h) % BS))
    return levels, sha(levels[0][:BS])


def data():
    return b''.join(bytes([b]) * BS for b in range(BLOCKS))


def digests(l6, corrupt=()):
    return b''.join(sha(l6[b * BS:(b + 1) * BS] if b not in corrupt else b'x' * BS) for b in range(BLOCKS))


class Tree:
    """The parts of update.Patch that tree_check reads; `reused` = {hash level index (0 = level 1):
    entries the update has the original release's for}."""

    def __init__(self, reused):
        self.lv, self.master = ivfc(data())
        self.levels, off = [], 0
        for level in self.lv:
            self.levels.append((off, len(level), BLOG))
            off += len(level)
        self.block, self.blocks, self.reused = BS, BLOCKS, reused
        self.hashes, self.known = self.hash_level(4)

    def hash_level(self, i):
        out, known = bytearray(self.lv[i]), bytearray(b'\x01') * (len(self.lv[i]) // 32)
        for e in self.reused.get(i, ()):
            out[e * 32:e * 32 + 32], known[e] = bytes(32), 0
        return bytes(out), known


class TreeCheckTests(unittest.TestCase):
    def test_all_hashes_carried(self):
        t = Tree({})
        self.assertEqual(update.tree_check(t, digests(data())), [])
        self.assertEqual(update.tree_check(t, digests(data(), corrupt={3})), [3])

    def test_level_5_reused_checked_at_level_4(self):
        t = Tree({4: range(4, 12)})
        self.assertEqual(t.known[4], 0)
        self.assertEqual(update.tree_check(t, digests(data())), [])
        # block 5's hash shares a level-5 block with block 4's: level 4 tells only that one is wrong
        self.assertEqual(update.tree_check(t, digests(data(), corrupt={5})), [4, 5])
        self.assertEqual(update.tree_check(t, digests(data(), corrupt={11})), [10, 11])

    def test_reused_on_two_levels_checked_at_level_3(self):
        t = Tree({4: range(4, 8), 3: (2, 3)})   # level 4's entries 2 and 3 are the hashes of 4-5 and 6-7
        self.assertEqual(update.tree_check(t, digests(data())), [])
        self.assertEqual(update.tree_check(t, digests(data(), corrupt={6})), [4, 5, 6, 7])

    def test_a_wrong_carried_block_does_not_take_its_neighbour_along(self):
        t = Tree({4: (4,)})
        self.assertEqual(update.tree_check(t, digests(data(), corrupt={5})), [5])   # 4 still confirmed

    def test_reused_up_to_level_1_checked_against_the_master_hash(self):
        t = Tree({4: range(BLOCKS), 3: range(8), 2: range(4), 1: range(2), 0: (0,)})
        self.assertEqual(update.tree_check(t, digests(data())), [])
        self.assertEqual(update.tree_check(t, digests(data(), corrupt={9})), list(range(BLOCKS)))


def patch_section(reused):
    """An update's RomFS section (BKTR) over the tree of data(): its own data is the image itself,
    except the virtual ranges in `reused`, which it takes from the original release."""
    levels, master = ivfc(data())
    image, offs = b''.join(levels), []
    for level in levels:
        offs.append(sum(len(x) for x in levels[:len(offs)]))
    pieces, at = [], 0
    for a, z in reused:
        pieces += [(at, at, 1), (a, 0x40, 0)]
        at = z
    pieces.append((at, at, 1))

    def table(entries, fmt, end):
        node = bytearray(0x4000)
        struct.pack_into('<IIQ', node, 0, 0, len(entries), end)
        size = struct.calcsize(fmt)
        for j, e in enumerate(entries):
            struct.pack_into(fmt, node, 0x10 + j * size, *e)
        head = bytearray(0x4000)
        struct.pack_into('<IIQ', head, 0, 0, 1, end)
        return bytes(head + node)
    ind_off = len(image)
    ind = table(pieces, '<QQi', len(image))
    ctr_off = ind_off + len(ind)
    ctr = table([(0, 0, 0)], '<QIi', ind_off)
    physical = image + ind + ctr
    fsh = bytearray(0x200)
    fsh[0x08:0x0C] = b'IVFC'
    for i, level in enumerate(levels):
        struct.pack_into('<QQI', fsh, 0x18 + i * 0x18, offs[i], len(level), BLOG)
    fsh[0xC8:0xE8] = master
    struct.pack_into('<QQ', fsh, 0x100, ind_off, len(ind))
    fsh[0x110:0x114] = b'BKTR'
    struct.pack_into('<QQ', fsh, 0x120, ctr_off, len(ctr))
    fsh[0x130:0x134] = b'BKTR'
    sec = types.SimpleNamespace(enc_type=4, fs_header=bytes(fsh),
                                read_raw=lambda off, size, ctr_hi=None: physical[off:off + size])
    nca = types.SimpleNamespace(content_type=0, section=lambda index: sec if index == 0 else None)
    return nca, offs


class ReusedHashesTests(unittest.TestCase):
    def open(self, reused_entries):
        l5 = sum(len(level) for level in ivfc(data())[0][:4])
        nca, _offs = patch_section([(l5 + a * 32, l5 + z * 32) for a, z in reused_entries])
        with mock.patch.object(update, 'nsp_ncas', lambda nsp, keydir: [('update.nca', nca)]):
            return update.Patch('update.nsp', 'keys')

    def test_update_opens_and_its_blocks_are_checked(self):
        """The user's case: level 5 is partly the original release's (main stopped with "bytes at
        ... come from the original release")."""
        p = self.open([(4, 12)])
        self.assertEqual([b for b in range(BLOCKS) if p.block_hash(b) is None], list(range(4, 12)))
        self.assertEqual(p.block_hash(3), sha(data()[3 * BS:4 * BS]))
        self.assertEqual(update.tree_check(p, digests(data())), [])
        self.assertEqual(update.tree_check(p, digests(data(), corrupt={7})), [6, 7])
        self.assertEqual(update.tree_check(p, digests(data(), corrupt={2})), [2])

    def test_a_block_without_its_own_hash_decides_nothing(self):
        p = self.open([(4, 12)])
        b = object.__new__(update.Builder)
        b.p = p
        self.assertIsNone(b.ok(5, {}))   # (first_pass skips it; verify checks it through level 4)


if __name__ == '__main__':
    unittest.main()
