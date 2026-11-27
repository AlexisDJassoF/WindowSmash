"""The mods' data.arc (runtime/mod_arc.c) on the player's own data.arc: tests/mod_arc_test.c, built
here with the host's C compiler, builds the archive for a mods folder made for each test (reading
data.arc through this test, over pipes), and tools/arc.py reads the result back the way the game
would: a mod's path gives the mod's bytes, the paths that shared its bytes give the originals, and
without mods nothing is served. Skipped without the game's data (dump/modules/romfs.cfg)."""
from pathlib import Path
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
CC = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
ROMFS_CFG = Path(os.environ.get('SSBU_ROMFS_CFG') or ROOT / 'dump' / 'modules' / 'romfs.cfg')

# A model file many colors share (decision 13), and its fighter's other colors
SHARED = 'fighter/mario/model/body/c{:02d}/model.numshb'
TEXTURE = 'fighter/mario/model/body/c{:02d}/def_mario_001_col.nutexb'


@unittest.skipUnless(sys.platform.startswith('linux') and CC and ROMFS_CFG.is_file(), 'Linux, a C compiler and the game\'s data')
class ModArcTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from arc import Arc
        from nca import RomfsImage, romfs_entries
        cls.temp = tempfile.TemporaryDirectory(prefix='mod-arc-test-')
        cls.exe = Path(cls.temp.name) / 'mod_arc_test'
        objs = []
        for src, flags in (('runtime/mod_zstd.c', ['-O2', '-w']), ('runtime/mod_arc.c', ['-O1']), ('runtime/mod_scan.c', ['-O1'])):
            obj = Path(cls.temp.name) / (Path(src).stem + '.o')
            r = subprocess.run([CC, *flags, '-D_GNU_SOURCE', '-I', str(ROOT / 'runtime'), '-c', str(ROOT / src), '-o', str(obj)],
                               capture_output=True, text=True)
            if r.returncode:
                raise AssertionError(f'compiling {src} failed:\n{r.stdout}{r.stderr}')
            objs.append(str(obj))
        r = subprocess.run([CC, '-O1', '-D_GNU_SOURCE', '-I', str(ROOT / 'runtime'), str(ROOT / 'tests/mod_arc_test.c'), *objs,
                            '-o', str(cls.exe)], capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(f'compiling the test failed:\n{r.stdout}{r.stderr}')
        r = subprocess.run([CC, '-O1', '-I', str(ROOT / 'runtime'), str(ROOT / 'tests/bntx_test.c'), str(ROOT / 'runtime/gpu_bntx.c'),
                            '-o', str(Path(cls.temp.name) / 'bntx_test')], capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(f'compiling the BNTX test failed:\n{r.stdout}{r.stderr}')
        cls.image = RomfsImage(str(ROMFS_CFG))
        (cls.arc_off, cls.arc_size), = [(o, s) for p, o, s in romfs_entries(cls.image) if p == 'data.arc']
        cls.arc = Arc(lambda off, size: cls.image.read(cls.arc_off + off, size))

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        d = tempfile.TemporaryDirectory(prefix='mod-arc-run-')
        self.addCleanup(d.cleanup)
        self.dir = Path(d.name)
        self.mods = self.dir / 'mods'
        self.mods.mkdir()

    def put(self, mod, path, data):
        p = self.mods / mod / path
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)

    def build(self, cache='-', **env):
        """(the archive's reader or None, the log)"""
        out = self.dir / 'out.bin'
        base = {**{k: v for k, v in os.environ.items() if not k.startswith('SSBU_')}, **env}
        p = subprocess.Popen([str(self.exe), str(self.mods), str(self.arc_size), str(cache), str(out)],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=base)
        status = None
        while True:
            kind = p.stdout.read(1)
            if kind == b'R':
                at, size = struct.unpack('<QQ', p.stdout.read(16))
                p.stdin.write(self.image.read(self.arc_off + at, size))
                p.stdin.flush()
            elif kind == b'E':
                status = struct.unpack('<i', p.stdout.read(4))[0]
                break
            else:
                break
        log = p.stderr.read().decode()
        p.wait(timeout=60)
        self.assertEqual(p.returncode, 0, log)
        self.assertIsNotNone(status, log)
        if not status:
            return None, log
        blob = out.read_bytes()
        self.assertEqual(blob[:8], b'SSBUMODT')
        n, total = struct.unpack_from('<QQ', blob, 8)
        header, tail = blob[24:24 + 0x38], blob[24 + 0x38:]
        self.assertEqual(n, self.arc_size)
        self.assertEqual(len(tail), total - n)

        def read(off, size):
            got = bytearray()
            while size:
                if off < 0x38:
                    k = min(size, 0x38 - off); got += header[off:off + k]
                elif off < n:
                    k = min(size, n - off); got += self.image.read(self.arc_off + off, k)
                else:
                    k = size; got += tail[off - n:off - n + k]
                    if off - n + k > len(tail):
                        raise AssertionError(f'read past the archive: {off:#x} +{size:#x}')
                off += k; size -= k
            return bytes(got)
        from arc import Arc
        return Arc(read), log

    def existing(self, pattern, colors=range(8)):
        return [pattern.format(c) for c in colors if self.arc.locate(pattern.format(c))]

    def test_no_mods_serves_nothing(self):
        self.assertIsNone(self.build()[0])
        self.put('.Disabled', SHARED.format(0), b'x' * 100)
        self.assertIsNone(self.build()[0])

    def test_empty_file_is_not_loaded(self):
        path = SHARED.format(0)
        if not self.arc.locate(path):
            self.skipTest('no Mario c00 model')
        self.put('Half Downloaded', path, b'')
        check, log = self.build()
        self.assertIsNone(check, log)
        self.assertIn(f'[mods] Half Downloaded: {path} is empty; not loaded', log)

    def test_shared_model_replaced_for_its_color_only(self):
        paths = self.existing(SHARED)
        if len(paths) < 2:
            self.skipTest('no shared Mario model in this data.arc')
        mine = os.urandom(len(self.arc.get(paths[0])) * 3)   # (three times the original's size)
        self.put('Big Mario', paths[0], mine)
        check, log = self.build()
        self.assertIsNotNone(check, log)
        self.assertEqual(check.get(paths[0]), mine, log)
        for other in paths[1:]:
            self.assertEqual(check.get(other), self.arc.get(other), f'{other} changed\n{log}')

    def test_several_files_and_a_missing_path(self):
        paths = self.existing(TEXTURE, [0]) + self.existing(SHARED, [0])
        if len(paths) < 2:
            self.skipTest('no Mario c00 files in this data.arc')
        data = {p: os.urandom(1000 + 7 * i) for i, p in enumerate(paths)}
        for p, d in data.items():
            self.put('Skin', p, d)
        self.put('Skin', 'fighter/mario/model/body/c00/extra.nutexb', b'new')
        self.put('Skin', 'info.toml', b'display_name = "Skin"\n')
        check, log = self.build()
        self.assertIsNotNone(check, log)
        for p, d in data.items():
            self.assertEqual(check.get(p), d, p)
        self.assertIn('[mods] Skin: fighter/mario/model/body/c00/extra.nutexb is not in data.arc', log)
        self.assertNotIn('info.toml', log)
        for p in self.existing(TEXTURE, range(1, 8)):
            self.assertEqual(check.get(p), self.arc.get(p), p)

    def test_unchanged_file_is_left(self):
        path = SHARED.format(0)
        if not self.arc.locate(path):
            self.skipTest('no Mario c00 model')
        self.put('Same', path, self.arc.get(path))
        check, log = self.build()
        self.assertIsNone(check, log)

    def test_first_mod_wins(self):
        path = SHARED.format(0)
        if not self.arc.locate(path):
            self.skipTest('no Mario c00 model')
        self.put('A_Mario', path, b'a' * 500)
        self.put('B_Mario', path, b'b' * 700)
        check, log = self.build()
        self.assertEqual(check.get(path), b'a' * 500)
        self.assertIn("[mods] B_Mario: " + path + " is A_Mario's", log)

    def test_color_move_from_folder_name(self):
        src, dst = SHARED.format(0), SHARED.format(2)
        if not (self.arc.locate(src) and self.arc.locate(dst)):
            self.skipTest('no Mario c00/c02 models')
        self.put('mario-moved-c02', src, b'moved' * 100)
        check, log = self.build()
        self.assertEqual(check.get(dst), b'moved' * 100, log)
        self.assertEqual(check.get(src), self.arc.get(src))

    def test_color_move_takes_voice_sounds_and_camera(self):
        # A Lucina c03 mod moved to c05 (as aMiku would be): every path that names the color goes to c05.
        # The model and the portrait are loaded there; the voice, sounds and camera are logged at c05
        # (moved), loaded or not (a sound with no slot of its own stays the game's). Kirby's copy of
        # Lucina moves with her too (tests/test_mod_menu.py).
        loaded = ['fighter/lucina/model/body/c{:02d}/model.numdlb', 'ui/replace/chara/chara_3/chara_3_lucina_{:02d}.bntx']
        moved = ['sound/bank/fighter_voice/vc_lucina_c{:02d}.nus3audio', 'sound/bank/fighter/se_lucina_c{:02d}.nus3audio',
                 'camera/fighter/lucina/c{:02d}/j02win1.nuanmb']
        if not all(self.arc.locate(f.format(c)) for f in loaded + moved for c in (3, 5)):
            self.skipTest('Lucina\'s per-color files are not in this data.arc')
        for i, f in enumerate(loaded + moved):
            self.put('lucina-miku-c05', f.format(3), bytes([65 + i]) * 400)
        check, log = self.build()
        self.assertIsNotNone(check, log)
        for i, f in enumerate(loaded):
            self.assertEqual(check.get(f.format(5)), bytes([65 + i]) * 400, f.format(5))
            self.assertEqual(check.get(f.format(3)), self.arc.get(f.format(3)), f.format(3))
        for f in moved:
            self.assertIn(f.format(5), log)
            self.assertNotIn(f.format(3), log)

    def test_regional_file_replaced_in_every_region(self):
        # (SSBU_MODS_VOICES=0, where a sound with a free slot keeps being regional) region r reads
        # info-to-data i2d + r: a mod's file must reach every region, not region 0 only (the others kept an
        # empty placeholder and "Rabbit Hole" Palutena's match never loaded)
        from arc import hash40, zstd_decompress
        path = 'sound/bank/fighter_voice/vc_palutena_c01.tonelabel'
        if not self.arc.locate(path):
            self.skipTest('no Palutena voice in this data.arc')
        mine = os.urandom(500)
        self.put('Rabbit', path, mine)
        check, log = self.build(SSBU_MODS_VOICES='0')
        self.assertIsNotNone(check, log)
        got = []
        for (h, region), (off, stored, size, flags) in check.files_by_path().items():
            if h != hash40(path):
                continue
            raw = check.read(off, stored)
            got.append((region, (zstd_decompress(raw) if flags & 3 else raw) == mine))
        self.assertEqual(len(got), self.arc.regions, got)
        self.assertTrue(all(ok for _, ok in got), sorted(got))

    def test_sound_pair_goes_in_together_or_not_at_all(self):
        # (SSBU_MODS_VOICES=0) Mario's color 1 voice: the bank has a slot, the samples do not; the bank must
        # not go in alone (the game's samples with the mod's bank, or the other way: the match never loaded)
        from arc import hash40
        base = 'sound/bank/fighter_voice/vc_mario_c00'
        if not (self.arc.locate(base + '.nus3audio') and self.arc.locate(base + '.nus3bank')):
            self.skipTest('no Mario voice in this data.arc')
        self.put('Voice', base + '.nus3audio', os.urandom(5000))
        self.put('Voice', base + '.nus3bank', os.urandom(700))
        self.put('Voice', SHARED.format(0), b'm' * 300)   # (something else that loads)
        check, log = self.build(SSBU_MODS_VOICES='0')
        self.assertIn(f'[mods] Voice: {base}.nus3audio and its .nus3bank go together and the samples cannot be loaded; neither is', log)
        self.assertIsNotNone(check, log)
        for ext in ('.nus3audio', '.nus3bank'):
            mine = {r: v[1:] for (h, r), v in check.files_by_path().items() if h == hash40(base + ext)}
            orig = {r: v[1:] for (h, r), v in self.arc.files_by_path().items() if h == hash40(base + ext)}
            self.assertEqual(mine, orig, ext)

    def test_model_needing_a_new_texture_keeps_the_games(self):
        # (Splatsune Inku: its material names textures it brings as new files; the game waited for them)
        mat, mesh = 'fighter/mario/model/body/c00/model.numatb', SHARED.format(0)
        if not (self.arc.locate(mat) and self.arc.locate(mesh)):
            self.skipTest('no Mario c00 model')
        self.put('Needs', mat, b'\x00' * 16 + b'def_mario_new_emi\x00' + b'\x00' * 14)
        self.put('Needs', mesh, b'm' * 300)
        self.put('Needs', 'fighter/mario/model/body/c00/def_mario_new_emi.nutexb', b't' * 100)
        self.put('Stray', 'fighter/mario/model/body/c01/model.numatb', b'\x00' * 16 + b'nothing_new\x00' + b'\x00' * 4)
        self.put('Stray', 'fighter/mario/model/body/c01/leftover.nutexb', b'x' * 50)   # (named by nothing: no effect)
        check, log = self.build()
        self.assertIn('[mods] Needs: fighter/mario/model/body/c00: its material needs def_mario_new_emi.nutexb, a new file', log)
        self.assertNotIn('[mods] Stray: fighter/mario/model/body/c01: its material', log)
        self.assertEqual(check.get(mesh), self.arc.get(mesh))
        self.assertEqual(check.get('fighter/mario/model/body/c01/model.numatb'), b'\x00' * 16 + b'nothing_new\x00' + b'\x00' * 4)

    def test_new_filedata_for_a_child_with_no_slot(self):
        # mod-support 1.4h: Kirby's copy of Lucina's mask in color 6 shares color 1's and its slot (start
        # + k) is another folder's file; it gets a new FileData of its own in its directory's folder
        # (load type 1), read in the directory's bulk read; the colors it shared with keep the original.
        # SSBU_MODS_FRESH=0 turns it off.
        from arc import hash40
        path = 'fighter/kirby/model/copy_lucina_mask/c{:02d}/model.numdlb'
        if not self.arc.locate(path.format(5)):
            self.skipTest('no Kirby copy of Lucina in this data.arc')
        self.put('Mask', path.format(5), b'k' * 700)
        check, log = self.build(SSBU_MODS_FRESH='0')
        self.assertIsNone(check, log)
        check, log = self.build()
        self.assertIsNotNone(check, log)
        self.assertIn('new FileData for children with no slot of their own', log)
        self.assertEqual(check.get(path.format(5)), b'k' * 700)
        for c in (0, 3):
            self.assertEqual(check.get(path.format(c)), self.arc.get(path.format(c)))
        pi = next(i for i in range(check.tab['paths'][2] - 1, -1, -1) if check._entry('paths', i, '<Q')[0] & ((1 << 40) - 1) == hash40(path.format(5)))
        info = check._entry('info_indices', check._entry('paths', pi, '<Q')[0] >> 40, '<2I')[1]
        folder, data, lt = check._entry('info_to_datas', check._entry('infos', info, '<4I')[2], '<3I')
        self.assertEqual(lt >> 24, 1)
        self.assertGreaterEqual(data, self.arc.tab['datas'][2])   # (a new FileData)

    def test_sounds_get_no_new_filedata_with_voices_off(self):
        # SSBU_MODS_VOICES=0: Lucina's color 6 voice has no slot, so the pair stays the game's
        base = 'sound/bank/fighter_voice/vc_lucina_c05'
        if not self.arc.locate(base + '.nus3audio'):
            self.skipTest('no Lucina voices in this data.arc')
        self.put('Voice', base + '.nus3audio', os.urandom(6000))
        self.put('Voice', base + '.nus3bank', os.urandom(900))
        check, log = self.build(SSBU_MODS_VOICES='0')
        self.assertIsNone(check, log)
        self.assertIn('go together and the samples cannot be loaded; neither is', log)

    def test_crowd_cheer_stays_the_games(self):
        base = 'sound/bank/fighter_voice/vc_palutena_cheer_c01'
        if not self.arc.locate(base + '.nus3audio'):
            self.skipTest('no Palutena cheer in this data.arc')
        self.put('Rabbit', base + '.nus3audio', os.urandom(5000))
        self.put('Rabbit', base + '.nus3bank', os.urandom(700))
        check, log = self.build()
        self.assertIsNone(check, log)
        self.assertIn(f'[mods] Rabbit: {base}.nus3audio: a crowd cheer, not loaded yet', log)

    def test_voices_become_own_files_like_a_koopalings(self):
        # (mod-support 1.4i) the voice pair of a color with no slot becomes the color's own
        # files the way a Koopaling's voice is in data.arc: load type 1, one entry, not regional (flags 0x10)
        from arc import hash40
        base = 'sound/bank/fighter_voice/vc_lucina_c05'
        if not self.arc.locate(base + '.nus3audio'):
            self.skipTest('no Lucina voices in this data.arc')
        audio, bank = os.urandom(6000), os.urandom(900)
        self.put('Voice', base + '.nus3audio', audio)
        self.put('Voice', base + '.nus3bank', bank)
        check, log = self.build()
        self.assertIsNotNone(check, log)
        for path, mine in ((base + '.nus3audio', audio), (base + '.nus3bank', bank)):
            self.assertEqual(check.get(path), mine, path)
            pi = next(i for i in range(check.tab['paths'][2] - 1, -1, -1) if check._entry('paths', i, '<Q')[0] & ((1 << 40) - 1) == hash40(path))
            info = check._entry('info_indices', check._entry('paths', pi, '<Q')[0] >> 40, '<2I')[1]
            _p, _ii, i2d, flags = check._entry('infos', info, '<4I')
            self.assertEqual(flags & 0x308000, 0, f'{path}: flags {flags:#x}')
            self.assertEqual(check._entry('info_to_datas', i2d, '<3I')[2] >> 24, 1, path)
        for c in (0, 3):
            p = f'sound/bank/fighter_voice/vc_lucina_c{c:02d}.nus3audio'
            self.assertEqual(check.files_by_path().get((hash40(p), 1)), self.arc.files_by_path().get((hash40(p), 1)), p)

    def test_param_file_of_another_version_stays_the_games(self):
        # Splatsune Inku's fighter/common/param/effect.prc came from an older game: one of the game's param
        # names missing (and every index after it shifted); with it the match never loaded
        path = 'fighter/common/param/effect.prc'
        if not self.arc.locate(path):
            self.skipTest('no effect.prc in this data.arc')
        orig = self.arc.get(path)
        n = struct.unpack_from('<I', orig, 8)[0]
        older = orig[:8] + struct.pack('<I', n - 8) + orig[12:16 + n - 8] + orig[16 + n:]   # (its last name gone)
        self.put('Old', path, older)
        check, log = self.build()
        self.assertIsNone(check, log)
        self.assertIn(f'[mods] Old: {path} was made for another version of the game (1 of its names missing)', log)
        tweaked = orig[:-1] + bytes([orig[-1] ^ 1])   # (every name there, a value changed)
        self.put('Old', path, tweaked)
        check, log = self.build()
        self.assertEqual(check.get(path), tweaked, log)

    def test_folders_last_file_stays_last(self):
        # Splatsune Inku's copy_inkling_gun texture: its slot came right after the last file of Inkling c06's
        # folder, so it was laid out past it; the game reads a folder to the end of its last file, never got
        # the texture and the match never loaded. A file going there goes before the last file instead.
        from arc import hash40
        path = 'fighter/inkling/model/copy_inkling_gun/c06/def_inkling_girl_002_col.nutexb'
        if not self.arc.locate(path):
            self.skipTest('no Inkling copy gun in this data.arc')
        mine = os.urandom(1000)
        self.put('Ink', path, mine)
        check, log = self.build()
        self.assertEqual(check.get(path), mine, log)
        pi = next(i for i in range(check.tab['paths'][2] - 1, -1, -1) if check._entry('paths', i, '<Q')[0] & ((1 << 40) - 1) == hash40(path))
        info = check._entry('info_indices', check._entry('paths', pi, '<Q')[0] >> 40, '<2I')[1]
        folder, data, _ = check._entry('info_to_datas', check._entry('infos', info, '<4I')[2], '<3I')
        ends = {}
        for i in range(check.tab['info_to_datas'][2]):
            f, d, _ = check._entry('info_to_datas', i, '<3I')
            if f == folder:
                off4, stored, _, _ = check._entry('datas', d, '<4I')
                if stored:
                    ends[d] = off4 * 4 + stored
        self.assertLess(ends[data], max(ends.values()), 'the mod file is the folder\'s last')

    def test_new_files_from_config_json(self):
        # mod-support 1.4c: Splatsune Inku's material needs two textures that are not in data.arc; its
        # config.json adds them to Inkling c06's directory (new-dir-files). Each gets a path, FileInfoIndex,
        # FileInfo, info-to-data and FileData; the directory's children move past the table's FileInfo,
        # the new ones after them. The model loads with them (no "needs a new file" rule).
        import json
        from arc import hash40
        new = 'fighter/inkling/model/body/c{:02d}/def_inkling_c{:02d}_001_emi.nutexb'
        mat = 'fighter/inkling/model/body/c06/model.numatb'
        if not self.arc.locate(mat) or self.arc.locate(new.format(6, 6)):
            self.skipTest('no Inkling c06 model in this data.arc')
        tex, material = os.urandom(3000), b'\x00' * 16 + b'def_inkling_c06_001_emi\x00' + b'\x00' * 8
        self.put('Ink', new.format(6, 6), tex)
        self.put('Ink', mat, material)
        self.put('Ink', 'fighter/inkling/model/body/c06/not_listed.nutexb', b'n' * 40)
        self.put('Ink', 'config.json', json.dumps({'new-dir-files': {'fighter/inkling/c06': [new.format(6, 6), new.format(6, 99)]}}).encode())
        check, log = self.build()
        self.assertIsNotNone(check, log)
        self.assertNotIn('its material needs', log)
        self.assertIn('[mods] Ink: 1 new files in fighter/inkling/c06', log)
        self.assertIn('not_listed.nutexb is not in data.arc', log)
        self.assertEqual(check.get(new.format(6, 6)), tex, log)
        self.assertEqual(check.get(mat), material)
        # the directory's children: its old ones (the same files) and the new one, each a FileInfoIndex of it
        dirs = {check._entry('dir_hash_to_info', i, '<Q')[0] & ((1 << 40) - 1): check._entry('dir_hash_to_info', i, '<Q')[0] >> 40
                for i in range(check.tab['dir_hash_to_info'][2])}
        d = dirs[hash40('fighter/inkling/c06')]
        old_start, old_count = self.arc._entry('dirs', d, '<QQQQII')[4:]
        start, count = check._entry('dirs', d, '<QQQQII')[4:]
        self.assertEqual(count, old_count + 1)
        self.assertGreaterEqual(start, self.arc.tab['infos'][2])
        names = [check._entry('infos', start + k, '<4I')[0] for k in range(count)]
        self.assertEqual(names[:old_count], [self.arc._entry('infos', old_start + k, '<4I')[0] for k in range(old_count)])
        pi, ii, i2d, flags = check._entry('infos', start + old_count, '<4I')
        self.assertEqual(check._entry('paths', pi, '<Q')[0] & ((1 << 40) - 1), hash40(new.format(6, 6)))
        self.assertEqual(check._entry('info_indices', ii, '<2I'), (d, start + old_count))
        folder, data, lt = check._entry('info_to_datas', i2d, '<3I')
        self.assertEqual(lt >> 24, 1)
        self.assertEqual(flags, 0x1000)
        # (its folder's FileData moved past the table's with it: the new one at start + k, in the range)
        fstart, fcount = check._entry('folder_offsets', folder, '<QIIII')[3:]
        self.assertEqual((data, fcount), (fstart + old_count, old_count + 1))
        # (the game reads a folder forward, child by child: its files' offsets increase in child order, the
        # new ones last, and none goes past the folder's end)
        ends = []
        for k in range(count):
            f, dd, _ = check._entry('info_to_datas', check._entry('infos', start + k, '<4I')[2], '<3I')
            off4, stored, _, _ = check._entry('datas', dd, '<4I')
            if f == folder and stored:
                ends.append((off4 * 4, off4 * 4 + stored))
        self.assertEqual(ends, sorted(ends))
        self.assertLessEqual(ends[-1][1], check._entry('folder_offsets', folder, '<QIII')[2])
        self.assertEqual(ends[-1][1], max(e for _, e in ends))   # (a texture: as every .nutexb of data.arc)
        for k in (0, old_count // 2, old_count - 1):   # (the moved children's FileInfoIndex follow them)
            own_ii = check._entry('infos', start + k, '<4I')[1]
            dd, info = check._entry('info_indices', own_ii, '<2I')
            if dd == d:
                self.assertEqual(info, start + k)
        for p in ('fighter/inkling/model/body/c06/model.numdlb', 'fighter/inkling/model/body/c05/model.numatb'):
            if self.arc.locate(p):
                self.assertEqual(check.get(p), self.arc.get(p), p)
        check, log = self.build(SSBU_MODS_NEW='0')
        self.assertIn('a new file (SSBU_MODS_NEW=0: not loaded)', log)
        self.assertIn('its material needs def_inkling_c06_001_emi.nutexb, a new file config.json does not add', log)
        self.assertIsNone(check, log)   # (then the material stays the game's too: nothing left to serve)

    def test_new_files_move_with_the_color(self):
        import json
        new = 'fighter/inkling/model/body/c{:02d}/def_inkling_c06_001_emi.nutexb'
        if not self.arc.locate('fighter/inkling/model/body/c07/model.numatb') or self.arc.locate(new.format(7)):
            self.skipTest('no Inkling c07 model in this data.arc')
        tex = os.urandom(2000)
        self.put('Ink-c07', new.format(6), tex)
        self.put('Ink-c07', 'fighter/inkling/model/body/c06/model.numatb', b'\x00' * 16 + b'def_inkling_c06_001_emi\x00')
        self.put('Ink-c07', 'config.json', json.dumps({'new-dir-files': {'fighter/inkling/c06': [new.format(6)]}}).encode())
        check, log = self.build()
        self.assertIn('1 new files in fighter/inkling/c07', log)
        self.assertEqual(check.get(new.format(7)), tex, log)
        self.assertIsNone(check.locate(new.format(6)))

    def test_moved_files_named_by_their_color_are_new_there(self):
        # Splatsune Inku moved from c06: Inkling's textures are named by the color (def_inkling_c06_...),
        # and c07 has none of those names - they are new files of c07's directory, no config.json needed
        tex = 'fighter/inkling/model/body/c{:02d}/def_inkling_c06_001_col.nutexb'
        if not self.arc.locate(tex.format(6)) or self.arc.locate(tex.format(7)):
            self.skipTest('no Inkling c06 textures in this data.arc')
        data = os.urandom(2500)
        self.put('Ink-c07', tex.format(6), data)
        self.put('Ink-c07', 'fighter/inkling/model/body/c06/model.numatb', b'\x00' * 16 + b'def_inkling_c06_001_col\x00')
        check, log = self.build()
        self.assertNotIn('is not in data.arc', log)
        self.assertNotIn('its material needs', log)
        self.assertIn('1 new files in fighter/inkling/c07', log)
        self.assertEqual(check.get(tex.format(7)), data, log)
        self.assertEqual(check.get(tex.format(6)), self.arc.get(tex.format(6)))   # (c06 the game's)

    def test_a_move_takes_its_ink_along_and_keeps_the_body(self):
        # Splatsune Inku (Inkling c06) moved to c00: Inkling's ink is fighter/common/param/effect.prc's, a
        # list with an entry a color; the mod's c06 entry goes to c00, c06 gets the game's back. A move to
        # c05 (the boy's body: the fight's loading hung) is undone at the start.
        import sys
        sys.path.insert(0, str(ROOT / 'tests'))
        path = 'fighter/common/param/effect.prc'
        if not self.arc.locate(path) or not self.arc.locate('fighter/inkling/model/body/c05/model.nusktb'):
            self.skipTest('no effect.prc or Inkling in this data.arc')
        game = self.arc.get(path)
        def entries(b):   # the 8-entry lists of 3 floats: their values, by list
            out, n = {}, struct.unpack_from('<I', b, 8)[0]
            refs = 16 + n
            root = refs + struct.unpack_from('<I', b, 12)[0]
            hashes = [struct.unpack_from('<Q', b, 16 + 8 * i)[0] & ((1 << 40) - 1) for i in range(n // 8)]
            count, r = struct.unpack_from('<II', b, root + 1)
            for i in range(count):
                hi, off = struct.unpack_from('<II', b, refs + r + 8 * i)
                o = root + off
                if b[o] == 11 and struct.unpack_from('<I', b, o + 1)[0] == 8:
                    row = []
                    for k in range(8):
                        e = o + struct.unpack_from('<I', b, o + 5 + 4 * k)[0]
                        row.append(b[e:e + 24])
                    out[hashes[hi]] = row
            return out
        lists = entries(game)
        key = next(k for k, row in lists.items() if len(set(row)) > 1)
        mine = bytearray(game)
        e = game.index(lists[key][6])
        mine[e + 10:e + 14] = struct.pack('<f', 0.123)   # (the mod's ink at c06: its first value changed)
        self.put('Ink-c00', path, bytes(mine))
        self.put('Ink-c00', 'fighter/inkling/model/body/c06/model.numdlb', self.arc.get('fighter/inkling/model/body/c06/model.numdlb'))
        (self.mods / 'windowsmash.toml').write_text('[mod."Ink-c00"]\ninkling.c06 = "c00"\n')
        check, log = self.build(TEST_GAME_HOOKS='1')
        self.assertRegex(log, r"effect.prc: \d+ of its colors' entries moved with them")
        got = entries(check.get(path))
        self.assertEqual(got[key][0], bytes(mine[e:e + 24]))   # (the mod's c06 entry, now c00's)
        self.assertEqual(got[key][6], lists[key][6])           # (c06: the game's again)
        (self.mods / 'windowsmash.toml').write_text('[mod."Ink-c00"]\ninkling.c06 = "c05"\n')
        check, log = self.build(TEST_GAME_HOOKS='1')
        self.assertIn('Ink-c00: moved to color 6, where Inkling has another body (skeleton); loaded at its own colors', log)
        self.assertNotIn('entries moved', log)

    def test_a_move_copies_the_games_textures_its_material_names(self):
        # Splatsune Inku's material names textures of c06 it does not bring - the game's (its hair's,
        # alp_inkling_c06_002_col, its eyes'); moved to c00, which has none of those names, the hair was
        # black. They are copied from the game's c06, new files of c00.
        tex = 'fighter/inkling/model/body/c{:02d}/alp_inkling_c06_002_col.nutexb'
        if not self.arc.locate(tex.format(6)) or self.arc.locate(tex.format(0)):
            self.skipTest('no Inkling c06 hair texture in this data.arc')
        self.put('Ink', 'fighter/inkling/model/body/c06/model.numatb',
                 b'\x00' * 16 + b'alp_inkling_c06_002_col\x00' + b'\x01' + b'default_Normal\x00' + b'\x00' * 8)
        (self.mods / 'windowsmash.toml').write_text('[mod."Ink"]\ninkling.c06 = "c00"\n')
        check, log = self.build(TEST_GAME_HOOKS='1')
        self.assertIn('Ink: fighter/inkling/model/body/c00/model.numatb: 1 of the game\'s textures it names copied with it', log)
        self.assertIn('new files in fighter/inkling/c00', log)
        self.assertEqual(check.get(tex.format(0)), self.arc.get(tex.format(6)), log)
        self.assertNotIn('its material needs', log)

    def test_a_move_takes_the_huds_ink_along(self):
        # Splatsune Inku's layout.arc (the fight's HUD): the ink behind the portrait is an animation whose
        # frames are the colors, the mod's c06 changed. Moved to c00, those keys go to c00's frames and
        # c06's get the game's back.
        path = 'ui/layout/info/info_melee/info_melee/layout.arc'
        anim = 'anim/info_melee_lct_ink_ink_bg.bflan'
        if not self.arc.locate(path):
            self.skipTest('no HUD layout in this data.arc')
        game = self.arc.get(path)
        def bflan(b):   # where the animation is in the SARC
            data, count = struct.unpack_from('<I', b, 0xC)[0], struct.unpack_from('<H', b, 0x1A)[0]
            sfnt = 0x20 + 16 * count
            for i in range(count):
                _, attr, st, en = struct.unpack_from('<4I', b, 0x20 + 16 * i)
                no = sfnt + 8 + 4 * (attr & 0xFFFF)
                if b[no:b.index(0, no)].decode() == anim:
                    return data + st
        def keys(b):   # the first curve of 15 keys: where its keys are
            f = bflan(b)
            p = f + struct.unpack_from('<H', b, f + 6)[0]
            while b[p:p + 4] != b'pai1':
                p += struct.unpack_from('<I', b, p + 4)[0]
            n, table = struct.unpack_from('<H', b, p + 0xE)[0], p + struct.unpack_from('<I', b, p + 0x10)[0]
            for e in range(n):
                eo = p + struct.unpack_from('<I', b, table + 4 * e)[0]
                for t in range(b[eo + 28]):
                    to = eo + struct.unpack_from('<I', b, eo + 32 + 4 * t)[0]
                    for c in range(b[to + 4]):
                        co = to + struct.unpack_from('<I', b, to + 8 + 4 * c)[0]
                        if struct.unpack_from('<H', b, co + 4)[0] == 15:
                            return co + struct.unpack_from('<I', b, co + 8)[0]
        k = keys(game)
        value = lambda b, i: struct.unpack_from('<f', b, k + 12 * i + 4)[0]
        mine = bytearray(game)
        for i in (12, 13):   # (color 6: the keys at frames 6 and 7)
            struct.pack_into('<f', mine, k + 12 * i + 4, 42.0)
        self.put('Ink', path, bytes(mine))
        self.put('Ink', 'fighter/inkling/model/body/c06/model.numdlb', self.arc.get('fighter/inkling/model/body/c06/model.numdlb'))
        (self.mods / 'windowsmash.toml').write_text('[mod."Ink"]\ninkling.c06 = "c00"\n')
        check, log = self.build(TEST_GAME_HOOKS='1')
        self.assertRegex(log, r"layout.arc: \d+ of its colors' entries moved with them")
        # (every curve of the colors moves c06 whole - those the mod left as the game's c06 had too: the
        # gauge's blue, green at c00 when ink was spent)
        out = check.get(path)
        self.assertEqual(value(out, 14), value(game, 14))   # (c07 untouched)
        self.assertEqual([value(out, i) for i in (0, 1)], [42.0, 42.0])
        self.assertEqual([value(out, i) for i in (12, 13)], [value(game, 12), value(game, 13)])

    def test_own_file_not_at_its_slot_is_replaced_where_it_is(self):
        # Wii Fit c01 keeps 407 of its own files (load type 1) at a FileData that is not start + k, its
        # voice among them; they were skipped ("load type 1; skipped"), now replaced in place
        base = 'sound/bank/fighter_voice/vc_wiifit_c01'
        if not self.arc.locate(base + '.nus3audio'):
            self.skipTest('no Wii Fit voices in this data.arc')
        audio, bank = os.urandom(7000), os.urandom(800)
        self.put('Miku', base + '.nus3audio', audio)
        self.put('Miku', base + '.nus3bank', bank)
        check, log = self.build()
        self.assertNotIn('skipped', log)
        self.assertEqual(check.get(base + '.nus3audio'), audio, log)
        self.assertEqual(check.get(base + '.nus3bank'), bank, log)
        from arc import hash40
        for path in (base + '.nus3audio', base + '.nus3bank'):   # (not regional any more: as a Koopaling's voice)
            pi = next(i for i in range(check.tab['paths'][2] - 1, -1, -1) if check._entry('paths', i, '<Q')[0] & ((1 << 40) - 1) == hash40(path))
            info = check._entry('info_indices', check._entry('paths', pi, '<Q')[0] >> 40, '<2I')[1]
            self.assertEqual(check._entry('infos', info, '<4I')[3] & 0x8000, 0, path)
        other = hash40('sound/bank/fighter_voice/vc_wiifit_c00.nus3audio')
        self.assertEqual(check.files_by_path().get((other, 1)), self.arc.files_by_path().get((other, 1)))

    def test_voice_bank_gets_the_games_id(self):
        # Miku Fitness Boxing's banks for Wii Fit's male colors are copies of the female's: ID 4048 where
        # the game looks for 4049 (vc_wiifit_c01), and the voice never played. As ARCropolis does, the
        # mod's bank is served with the game's ID, the rest of it as it is.
        from arc import hash40, zstd_decompress
        base = 'sound/bank/fighter_voice/vc_wiifit_c{:02d}'
        fb = self.arc.files_by_path()

        def game(path):
            loc = fb.get((hash40(path), 1)) or fb.get((hash40(path), 0))
            if not loc or not loc[1]:
                return None
            raw = self.arc.read(loc[0], loc[1])
            return zstd_decompress(raw) if loc[3] & 3 else raw
        female, male = game(base.format(0) + '.nus3bank'), game(base.format(1) + '.nus3bank')
        if not female or not male:
            self.skipTest('no Wii Fit voices in this data.arc')
        i, j = female.find(b'vc_wiifit\x00'), male.find(b'vc_wiifit_c01\x00')
        self.assertGreater(i, 0)
        self.assertGreater(j, 0)
        self.put('Miku', base.format(1) + '.nus3audio', os.urandom(5000))
        self.put('Miku', base.format(1) + '.nus3bank', female)
        check, log = self.build()
        self.assertIn(f'[mods] Miku: {base.format(1)}.nus3bank: served with the game\'s bank name and ID ("vc_wiifit_c01"', log)
        got = check.get(base.format(1) + '.nus3bank')

        def sections(b):   # (NUS3, size, BANKTOC, TOC size, count, (magic, size)..., then each section)
            tsize, n = struct.unpack_from('<II', b, 16)
            out, p = {}, 20 + tsize
            for k in range(n):
                magic, size = struct.unpack_from('<4sI', b, 24 + 8 * k)
                self.assertEqual(b[p:p + 4], magic)
                out[magic] = b[p + 8:p + 8 + size]
                p += 8 + size
            self.assertEqual(p, len(b))
            self.assertEqual(struct.unpack_from('<I', b, 4)[0], len(b) - 8)
            return out
        g, m, f = sections(got), sections(male), sections(female)
        self.assertEqual(g[b'BINF'], m[b'BINF'])   # (the game's name and ID)
        self.assertEqual(list(g), list(f))
        for k in f:
            if k not in (b'BINF', b'JUNK'):
                self.assertEqual(g[k], f[k], k)   # (the rest the mod's)
        self.build(cache=self.dir / 'cache.bin')
        check3, log3 = self.build(cache=self.dir / 'cache.bin')   # (from the cache: the same)
        self.assertIn('the table from', log3)
        self.assertEqual(check3.get(base.format(1) + '.nus3bank'), got)

    def test_cache(self):
        path = SHARED.format(0)
        if not self.arc.locate(path):
            self.skipTest('no Mario c00 model')
        cache = self.dir / 'table.bin'
        self.put('Skin', path, b'1' * 300)
        first, log = self.build(cache)
        self.assertIn('cache=0', log)
        self.assertTrue(cache.is_file())
        again, log = self.build(cache)
        self.assertIn('cache=1', log)
        self.assertEqual(again.get(path), b'1' * 300)
        self.put('Skin', path, b'2' * 301)   # (a new size: built again)
        changed, log = self.build(cache)
        self.assertIn('cache=0', log)
        self.assertEqual(changed.get(path), b'2' * 301)


    # ---- one file by its path (mod_arc_read_path), and the BNTX reader (runtime/gpu_bntx.c) ----

    def read_paths(self, *paths):
        """{path: bytes or None} as mod_arc_read_path gives them"""
        out = self.dir / 'paths.bin'
        p = subprocess.Popen([str(self.exe), '--path', str(out), *paths], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
        while True:
            kind = p.stdout.read(1)
            if kind == b'R':
                at, size = struct.unpack('<QQ', p.stdout.read(16))
                p.stdin.write(self.image.read(self.arc_off + at, size))
                p.stdin.flush()
            else:
                self.assertEqual(kind, b'E', p.stderr.read().decode())
                self.assertEqual(struct.unpack('<i', p.stdout.read(4))[0], 1)
                break
        p.wait(timeout=60)
        blob, got, at = out.read_bytes(), {}, 0
        for path in paths:
            n, = struct.unpack_from('<Q', blob, at)
            at += 8
            if n == (1 << 64) - 1:
                got[path] = None
            else:
                got[path] = blob[at:at + n]
                at += n
        return got

    def test_read_a_file_by_its_path(self):
        mario = 'ui/replace/chara/chara_7/chara_7_mario_00.bntx'
        sora = 'ui/replace_patch/chara/chara_7/chara_7_trail_00.bntx'   # (a DLC's: in replace_patch alone)
        model = SHARED.format(3)
        got = self.read_paths(mario, sora, model, 'ui/replace/chara/chara_7/chara_7_trail_00.bntx', 'no/such/file.bntx')
        for path in (mario, sora, model):
            self.assertEqual(got[path], self.arc.get(path), path)
        self.assertIsNone(got['ui/replace/chara/chara_7/chara_7_trail_00.bntx'])
        self.assertIsNone(got['no/such/file.bntx'])

    @staticmethod
    def unswizzle(bntx):
        """The BNTX's level 0 as linear 4x4 BC blocks, read here on its own (tools/arc.py's way of
        reading the format, the block-linear GOBs written out again)"""
        nx = bntx.find(b'NX  ')
        brti, = struct.unpack_from('<Q', bntx, struct.unpack_from('<Q', bntx, nx + 8)[0])
        w, h = struct.unpack_from('<II', bntx, brti + 0x24)
        bh = struct.unpack_from('<I', bntx, brti + 0x34)[0] & 7
        mip0, = struct.unpack_from('<Q', bntx, struct.unpack_from('<Q', bntx, brti + 0x70)[0])
        row, rows = (w + 3) // 4 * 16, (h + 3) // 4
        out = bytearray(row * rows)
        for y in range(rows):
            by, yin = divmod(y, 8 << bh)
            line = by * ((row + 63) // 64) * (512 << bh) + (yin // 8) * 512
            for x in range(0, row, 16):
                g = ((x % 64) // 32) * 256 + ((yin % 8) // 2) * 64 + ((x % 32) // 16) * 32 + (yin % 2) * 16
                off = mip0 + line + (x // 64) * (512 << bh) + g
                out[y * row + x:y * row + x + 16] = bntx[off:off + 16]
        return bytes(out)

    def test_bntx_portrait_reads_as_a_picture(self):
        for path, size in (('ui/replace/chara/chara_7/chara_7_mario_00.bntx', (454, 300)),
                           ('ui/replace/chara/chara_2/chara_2_mario_00.bntx', (64, 64)),
                           ('ui/replace/chara/chara_3/chara_3_mario_00.bntx', (1132, 1212))):
            bntx, raw = self.dir / 'p.bntx', self.dir / 'p.raw'
            data = self.arc.get(path)
            bntx.write_bytes(data)
            r = subprocess.run([str(Path(self.temp.name) / 'bntx_test'), str(bntx), str(raw)], capture_output=True, text=True, timeout=30)
            info = dict(kv.split('=') for kv in r.stdout.split())
            self.assertEqual((info['format'], int(info['width']), int(info['height'])), ('3', *size), r.stdout)   # (OVERLAY_TEX_BC7)
            blocks = raw.read_bytes()
            self.assertEqual(blocks, self.unswizzle(data), path)
            try:   # (and, with PIL, a real picture: opaque in the middle, many colours)
                from PIL import Image
            except ImportError:
                continue
            w, h = size
            dds = (b'DDS ' + struct.pack('<7I', 124, 0x81007, h, w, len(blocks), 0, 1) + bytes(44) + struct.pack('<2I', 32, 4) + b'DX10'
                   + bytes(20) + struct.pack('<4I', 0x1000, 0, 0, 0) + bytes(4) + struct.pack('<5I', 98, 3, 0, 1, 0))
            (self.dir / 'p.dds').write_bytes(dds + blocks)
            pic = Image.open(self.dir / 'p.dds').convert('RGBA')
            self.assertEqual(pic.size, size)
            self.assertGreater(pic.getpixel((w // 2, h // 2))[3], 200, path)
            self.assertGreater(len(set(pic.getdata())), 500, path)
        r = subprocess.run([str(Path(self.temp.name) / 'bntx_test'), str(self.dir / 'p.raw'), str(self.dir / 'x')], capture_output=True, text=True)
        self.assertIn('error=not a BNTX file', r.stdout)
        (self.dir / 'cut.bntx').write_bytes(data[:0x2000])
        r = subprocess.run([str(Path(self.temp.name) / 'bntx_test'), str(self.dir / 'cut.bntx'), str(self.dir / 'x')], capture_output=True, text=True)
        self.assertIn('error=cut short', r.stdout)

if __name__ == '__main__':
    unittest.main()
