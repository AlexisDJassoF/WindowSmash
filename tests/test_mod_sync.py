"""The room's mods on the network (runtime/mod_sync.c, mod_fetch.c, mod_zip.c; openspec change
netplay-mod-sync): the manifest asked for and sent in chunks, the files in blocks with some lost on the
way, the host's rate and its fights, a host that is an older game or goes away, a block changed on the
way, cancelling, the player's own copy of a file and a mod's .zip from where it comes from. Built with
tests/mod_sync_test.c, which is host and guest in one process over a network of its own."""
from pathlib import Path
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile

ROOT = Path(__file__).resolve().parent.parent
CC = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')

SKIN = 'fighter/palutena/model/body/c03/model.numdlb'
PORTRAIT = 'ui/replace/chara/chara_3/chara_3_palutena_03.bntx'
PARAM = 'fighter/mario/param/fighter_param.prc'


@unittest.skipUnless(sys.platform.startswith('linux') and CC, 'Linux and a C compiler')
class SyncTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='mod-sync-test-')
        cls.exe = Path(cls.temp.name) / 'mod_sync_test'
        srcs = ['tests/mod_sync_test.c', 'runtime/mod_sync.c', 'runtime/mod_fetch.c', 'runtime/mod_zip.c', 'runtime/mod_manifest.c',
                'runtime/mod_sha256.c', 'runtime/mod_scan.c']
        r = subprocess.run([CC, '-O1', '-g', '-w', '-D_GNU_SOURCE', '-pthread', '-I', str(ROOT / 'runtime'), *[str(ROOT / s) for s in srcs],
                            '-o', str(cls.exe)], capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(f'compiling the test failed:\n{r.stdout}{r.stderr}')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        d = tempfile.TemporaryDirectory(prefix='mod-sync-run-')
        self.addCleanup(d.cleanup)
        self.dir = Path(d.name)
        self.mods = self.dir / 'mods'
        self.blobs = self.dir / 'blobs'
        self.mods.mkdir()
        self.blobs.mkdir()

    def mod(self, folder, files, info=None):
        for path, data in files.items():
            p = self.mods / folder / path
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        if info:
            (self.mods / folder / 'info.toml').write_text(info)

    def two_mods(self, size=300_000):
        skin, portrait, param = os.urandom(size), os.urandom(5000), os.urandom(1500)
        self.mod('Skin', {SKIN: skin, PORTRAIT: portrait})
        self.mod('Physics', {PARAM: param})
        return {hashlib.sha256(b).hexdigest(): b for b in (skin, portrait, param)}

    def run_sync(self, scenario='full', timeout=60, **env):
        base = {k: v for k, v in os.environ.items() if not k.startswith(('SSBU_', 'LOSS', 'FIGHT', 'RATE', 'OWN'))}
        r = subprocess.run([str(self.exe), str(self.mods), str(self.blobs), scenario], capture_output=True, text=True,
                           env={**base, **{k: str(v) for k, v in env.items()}}, timeout=timeout)
        self.assertEqual(r.returncode, 0, r.stderr)
        out = {'files': {}, 'log': r.stderr}
        for line in r.stdout.splitlines():
            w = line.split(' ', 6)
            if w[0] == 'manifest':
                out['manifest'] = dict(state=int(w[1]), mods=int(w[2]), files=int(w[3]), bytes=int(w[4]), ms=int(w[5]))
            elif w[0] == 'file':
                out['files'][w[1]] = (int(w[2]), line.split(' ', 3)[3] if len(w) > 3 else '')
            elif w[0] == 'done':
                out['done'] = dict(state=int(w[1]), failed=int(w[2]), ms=int(w[3]), given=int(w[4]), paused=int(w[5]), saw_pause=int(w[6]))
            elif w[0] == 'gameplay':
                out['gameplay'] = (int(w[1]), line.split(' ', 2)[2] if len(w) > 2 else '')
        return out

    def blobs_ok(self, want):
        for h, data in want.items():
            self.assertEqual((self.blobs / h).read_bytes(), data, h)
        self.assertEqual([p.name for p in self.blobs.iterdir() if p.name.endswith('.part')], [])

    # MQ_* and FS_*, FR_* as in mod_sync.h and mod_fetch.h
    GOT, NONE = 2, 3
    DONE, CANCELLED = 2, 3
    OK, FAILED = 1, 2

    def test_manifest_and_files(self):
        want = self.two_mods()
        out = self.run_sync()
        self.assertEqual((out['manifest']['state'], out['manifest']['mods'], out['manifest']['files']), (self.GOT, 2, 3))
        self.assertEqual(out['done']['state'], self.DONE)
        self.assertEqual({h: r for h, (r, _) in out['files'].items()}, {h: self.OK for h in want})
        self.blobs_ok(want)
        self.assertIn('[mods] room manifest (private room): 2 mods', out['log'])

    def test_a_big_manifest_in_many_chunks(self):
        files = {f'fighter/mario/model/body/c0{i % 8}/part{i:04d}_with_a_long_name_to_make_the_manifest_big.numdlb': bytes([i % 256]) * (i + 1)
                 for i in range(1200)}
        self.mod('Many', files)
        out = self.run_sync(LOSS=0.05)
        self.assertEqual((out['manifest']['state'], out['manifest']['files']), (self.GOT, 1200))   # (~140 KB: many MODLISTs, some lost)
        self.assertEqual(out['done']['failed'], 0)

    def test_lost_packets_asked_again(self):
        want = self.two_mods(size=600_000)
        out = self.run_sync(LOSS=0.15)
        self.assertEqual(out['manifest']['state'], self.GOT)
        self.assertEqual((out['done']['state'], out['done']['failed']), (self.DONE, 0))
        self.blobs_ok(want)

    def test_older_host_gives_up_after_3_s(self):
        self.two_mods()
        t = time.monotonic()
        out = self.run_sync('old')
        self.assertEqual(out['manifest']['state'], self.NONE)
        self.assertGreaterEqual(out['manifest']['ms'], 2900)
        self.assertLess(time.monotonic() - t, 6)

    def test_no_mods(self):
        out = self.run_sync()
        self.assertEqual((out['manifest']['state'], out['manifest']['mods']), (self.GOT, 0))

    def test_rate(self):
        want = self.two_mods(size=1_000_000)
        out = self.run_sync(RATE=512 << 10)
        self.assertGreaterEqual(out['done']['ms'], 1500)   # (about 1 MB at 512 KB/s, its first tenth of a second's worth at once)
        self.blobs_ok(want)

    def test_fight_pauses_the_host(self):
        want = self.two_mods()
        out = self.run_sync(FIGHT=2)
        self.assertGreater(out['done']['paused'], 0)
        self.assertEqual(out['done']['saw_pause'], 1)
        self.assertGreaterEqual(out['done']['ms'], 1500)   # (nothing came while the fight went on)
        self.assertEqual(out['done']['failed'], 0)
        self.blobs_ok(want)

    def test_a_changed_block_fails_its_file(self):
        want = self.two_mods()
        out = self.run_sync('corrupt')
        failed = [h for h, (r, _) in out['files'].items() if r == self.FAILED]
        self.assertEqual(len(failed), 1)
        self.assertIn('SHA-256', out['files'][failed[0]][1])
        self.assertFalse((self.blobs / failed[0]).exists())
        for h in want:   # (the others came)
            if h not in failed:
                self.assertEqual((self.blobs / h).read_bytes(), want[h])
        self.assertEqual([p.name for p in self.blobs.iterdir() if p.name.endswith('.part')], [])

    def test_cancel_keeps_what_was_verified(self):
        self.two_mods(size=2_000_000)
        out = self.run_sync('cancel', RATE=1 << 20)
        self.assertEqual(out['done']['state'], self.CANCELLED)
        self.assertEqual([p.name for p in self.blobs.iterdir() if p.name.endswith('.part')], [])

    def test_host_gone(self):
        self.two_mods(size=2_000_000)
        out = self.run_sync('gone', RATE=1 << 20, SSBU_MODS_SILENT_MS=800)
        self.assertEqual(out['done']['state'], self.DONE)
        self.assertGreater(out['done']['failed'], 0)
        self.assertTrue(any('stopped answering' in why for _, why in out['files'].values()))

    def test_lobby_never_gives_gameplay_files(self):
        want = self.two_mods()
        param = next(h for h, d in want.items() if len(d) == 1500)
        out = self.run_sync('lobby', GAMEPLAY_SHA=param, GAMEPLAY_SIZE=1500)
        self.assertEqual(out['manifest']['files'], 2)   # (the skin's two; the parameters mod without its file)
        self.assertEqual(out['gameplay'][0], self.FAILED)
        self.assertIn('does not give', out['gameplay'][1])
        self.assertFalse((self.blobs / param).exists())

    def test_own_copy_is_not_downloaded(self):
        want = self.two_mods()
        mine = self.dir / 'my_skin.numdlb'
        mine.write_bytes(want[next(h for h, d in want.items() if len(d) == 300_000)])
        out = self.run_sync(OWN=mine)
        self.blobs_ok(want)
        self.assertLess(out['done']['given'], 300_000)   # (the big one copied from the player's own mod)

    def test_origin_zip(self):
        skin, portrait = os.urandom(200_000), b'portrait' * 2000
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w') as z:   # (its own folders: files are known by their hashes)
            z.writestr(zipfile.ZipInfo('Palutena Ice/readme.txt'), b'hello')
            z.writestr('Palutena Ice/fighter/palutena/model/body/c03/model.numdlb', skin, compress_type=zipfile.ZIP_DEFLATED)
            z.writestr('Palutena Ice/ui/x.bntx', portrait, compress_type=zipfile.ZIP_STORED)
        origin = self.dir / 'origin'
        origin.mkdir()
        (origin / 'pit.zip').write_bytes(buf.getvalue())
        zip_sha = hashlib.sha256(buf.getvalue()).hexdigest()
        self.mod('Skin', {SKIN: skin, PORTRAIT: portrait}, info=f'url = "https://example.org/dl/pit.zip"\nurl_sha256 = "{zip_sha}"\n')
        out = self.run_sync(SSBU_MODS_ORIGIN_DIR=origin, NO_HOST_FILES=1, SSBU_MODS_SILENT_MS=500)
        self.assertEqual(out['done']['failed'], 0, out['files'])
        self.blobs_ok({hashlib.sha256(skin).hexdigest(): skin, hashlib.sha256(portrait).hexdigest(): portrait})
        self.assertIn('2 files from its .zip', out['log'])

    def test_origin_zip_with_a_wrong_hash_is_not_used(self):
        skin = os.urandom(50_000)
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
            z.writestr('model.numdlb', skin)
        origin = self.dir / 'origin'
        origin.mkdir()
        (origin / 'pit.zip').write_bytes(buf.getvalue())
        self.mod('Skin', {SKIN: skin}, info=f'url = "https://example.org/pit.zip"\nurl_sha256 = "{"0" * 64}"\n')
        out = self.run_sync(SSBU_MODS_ORIGIN_DIR=origin)   # (then from the host)
        self.assertIn('not the one the mod declares', out['log'])
        self.assertEqual(out['done']['failed'], 0)
        self.blobs_ok({hashlib.sha256(skin).hexdigest(): skin})


@unittest.skipUnless(sys.platform.startswith('linux') and CC, 'Linux and a C compiler')
class InflateTests(unittest.TestCase):
    """mod_zip.c's inflate against zlib's deflate: every level, and data that compresses well and badly"""
    def test_inflate(self):
        import zlib
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / 't.c'
            src.write_text('#include <stdio.h>\n#include <stdlib.h>\n#include <stdint.h>\n'
                           'int mod_inflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen);\n'
                           'int main(int c, char **v) { FILE *f = fopen(v[1], "rb"); static uint8_t in[1 << 22], out[1 << 22];\n'
                           ' size_t n = fread(in, 1, sizeof in, f); size_t want = (size_t)atol(v[2]);\n'
                           ' if (!mod_inflate(in, n, out, want)) { puts("bad"); return 0; } fwrite(out, 1, want, stdout); return 0; }\n')
            exe = Path(d) / 't'
            (Path(d) / 'rt.c').write_text('const char *rt_data_dir = ".";\n')   # (mod_scan.c's)
            srcs = ['runtime/mod_zip.c', 'runtime/mod_sha256.c', 'runtime/mod_fetch.c', 'runtime/mod_sync.c', 'runtime/mod_manifest.c', 'runtime/mod_scan.c']
            r = subprocess.run([CC, '-O1', '-w', '-D_GNU_SOURCE', '-pthread', '-I', str(ROOT / 'runtime'), str(src), str(Path(d) / 'rt.c'),
                                *[str(ROOT / x) for x in srcs], '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            samples = [b'', b'a', b'abc' * 10000, os.urandom(70000), bytes(range(256)) * 300, (b'the quick brown fox ' * 50 + os.urandom(30)) * 40]
            for data in samples:
                for level in (0, 1, 6, 9):
                    c = zlib.compressobj(level, zlib.DEFLATED, -15)
                    packed = c.compress(data) + c.flush()
                    (Path(d) / 'in').write_bytes(packed)
                    r = subprocess.run([str(exe), str(Path(d) / 'in'), str(len(data))], capture_output=True)
                    self.assertEqual(r.stdout, data, (len(data), level))
            (Path(d) / 'in').write_bytes(b'\xff' * 100)   # (not deflate)
            r = subprocess.run([str(exe), str(Path(d) / 'in'), '1000'], capture_output=True)
            self.assertEqual(r.stdout.strip(), b'bad')


if __name__ == '__main__':
    unittest.main()
