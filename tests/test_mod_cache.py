"""The rooms' mods kept here (runtime/mod_cache.c; openspec change netplay-mod-sync): a host's manifests one
after another against its index - new, the same, moved, changed, dropped and back -, the player's own
copies, turns of hosting, what the game loads in mirror and overlay, and forgetting old hosts. Built with
tests/mod_cache_test.c; the manifests come from tests/test_mod_manifest.py's encoder."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_mod_manifest import encode   # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
CC = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
A, B = 0xA000000000000001, 0xB000000000000002
SAME, MOVED, CHANGED, NEW, REMOVED, UNAVAILABLE, DROPPED = range(7)
DAY = 86400


def skin(name, fighter, color, data, to=None, extra=()):
    files = [(f'fighter/{fighter}/model/body/c{color:02d}/model.numdlb', data)] + list(extra)
    return {'name': name, 'flags': 2, 'fighter': fighter, 'to': to or [-1] * 8,
            'files': [(p, len(d), hashlib.sha256(d).digest()) for p, d in files], 'data': dict(files)}


def param(name, data):
    p = 'fighter/mario/param/fighter_param.prc'
    return {'name': name, 'flags': 3, 'fighter': 'mario', 'files': [(p, len(data), hashlib.sha256(data).digest())], 'data': {p: data}}


@unittest.skipUnless(sys.platform.startswith('linux') and CC, 'Linux and a C compiler')
class CacheTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='mod-cache-test-')
        cls.exe = Path(cls.temp.name) / 'mod_cache_test'
        srcs = ['tests/mod_cache_test.c', 'runtime/mod_cache.c', 'runtime/mod_fetch.c', 'runtime/mod_sync.c', 'runtime/mod_zip.c',
                'runtime/mod_manifest.c', 'runtime/mod_sha256.c', 'runtime/mod_scan.c']
        r = subprocess.run([CC, '-O1', '-g', '-w', '-D_GNU_SOURCE', '-pthread', '-I', str(ROOT / 'runtime'), *[str(ROOT / s) for s in srcs],
                            '-o', str(cls.exe)], capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(f'compiling the test failed:\n{r.stdout}{r.stderr}')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        d = tempfile.TemporaryDirectory(prefix='mod-cache-run-')
        self.addCleanup(d.cleanup)
        self.dir = Path(d.name)
        self.cache = self.dir / 'mods_join'
        self.cache.mkdir(parents=True)
        self.store = self.dir / 'store'   # (what the host would send: files by their hash)
        self.store.mkdir()
        self.own = self.dir / 'own'
        self.own.mkdir()

    def run_exe(self, *args):
        r = subprocess.run([str(self.exe), *map(str, args)], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout, r.stderr

    def manifest(self, mods, host=A, name='Alexis'):
        for m in mods:
            for d in m.get('data', {}).values():
                (self.store / hashlib.sha256(d).hexdigest()).write_bytes(d)
        f = self.dir / 'm.bin'
        f.write_bytes(encode([{k: v for k, v in m.items() if k != 'data'} for m in mods], host_id=host, host=name))
        return f

    def reconcile(self, mods, host=A, own=False):
        out, _ = self.run_exe('reconcile', self.cache, self.manifest(mods, host), *([self.own] if own else []))
        return self.parse(out)

    def accept(self, mods, host=A, now=None, own=False, name='Alexis'):
        out, _ = self.run_exe('accept', self.cache, self.manifest(mods, host, name), int(now or time.time()), self.store, *([self.own] if own else []))
        res = self.parse(out)
        self.assertIn('written 1', out)
        return res

    def parse(self, out):
        res = {'mods': {}, 'fetch': []}
        for line in out.splitlines():
            if line.startswith('rec '):
                state, rest = line[4:].split(' ', 1)
                name, nums = rest.rsplit('|', 1)
                files, nbytes, own, frm, to = nums.split()
                res['mods'][name] = dict(state=int(state), files=int(files), bytes=int(nbytes), own=int(own), frm=int(frm), to=int(to))
            elif line.startswith('total '):
                files, nbytes, own, dialog, missing = line.split()[1:]
                res.update(files=int(files), bytes=int(nbytes), own=int(own), dialog=int(dialog), missing=int(missing))
            elif line.startswith('fetch '):
                res['fetch'].append(line.split()[1:])
        return res

    def load(self, host, mode, own=False):
        out, _ = self.run_exe('load', self.cache, f'{host:016x}', mode, *([self.own] if own else []))
        return {l.split(' ', 2)[1]: l.split(' ', 2)[2] for l in out.splitlines() if l.startswith('load ')}   # dest: mod (and ' @dir' for a new file)

    def blobs(self):
        return sorted(p.name for p in (self.cache / '.blobs').iterdir())

    # ---- reconciling ------------------------------------------------------------------------------
    def test_first_visit_then_the_same(self):
        mods = [skin('Pikachu Ice', 'pikachu', 4, b'ice' * 100), skin('Link Dark', 'link', 5, b'dark' * 100), param('Physics Pro', b'prc' * 50)]
        r = self.reconcile(mods)
        self.assertEqual({n: m['state'] for n, m in r['mods'].items()}, {'Pikachu Ice': NEW, 'Link Dark': NEW, 'Physics Pro': NEW})
        self.assertEqual((r['files'], r['bytes'], r['dialog']), (3, 300 + 400 + 150, 1))
        self.accept(mods)
        self.assertEqual(len(self.blobs()), 3)
        r = self.reconcile(mods)
        self.assertEqual({m['state'] for m in r['mods'].values()}, {SAME})
        self.assertEqual((r['files'], r['dialog']), (0, 0))   # (nothing to fetch: no dialog)

    def test_moved_is_not_fetched_again(self):
        mods = [skin('Link Dark', 'link', 5, b'dark' * 100)]
        self.accept(mods)
        moved = [skin('Link Dark', 'link', 5, b'dark' * 100, to=[-1, -1, -1, -1, -1, 6, -1, -1])]
        r = self.reconcile(moved)
        self.assertEqual(r['mods']['Link Dark'], dict(state=MOVED, files=0, bytes=0, own=0, frm=5, to=6))
        self.assertEqual(r['dialog'], 0)
        self.accept(moved)
        self.assertEqual(self.load(A, 'mirror'), {'fighter/link/model/body/c06/model.numdlb': 'Link Dark @fighter/link/c06'})   # (moved: c06's directory, for files c06 lacks)

    def test_changed_fetches_only_the_new_files(self):
        portrait = ('ui/replace/chara/chara_3/chara_3_pikachu_04.bntx', b'face' * 100)
        self.accept([skin('Pikachu Ice', 'pikachu', 4, b'ice' * 100, extra=[portrait])])
        r = self.reconcile([skin('Pikachu Ice', 'pikachu', 4, b'ice v2' * 100, extra=[portrait])])
        self.assertEqual((r['mods']['Pikachu Ice']['state'], r['mods']['Pikachu Ice']['files'], r['mods']['Pikachu Ice']['bytes']), (CHANGED, 1, 600))
        self.assertEqual(r['dialog'], 1)

    def test_dropped_by_the_host_then_back(self):
        a, b = skin('Pikachu Ice', 'pikachu', 4, b'ice' * 100), skin('Kirby Pink', 'kirby', 1, b'pink' * 100)
        self.accept([a, b])
        r = self.reconcile([a])
        self.assertEqual(r['mods']['Kirby Pink']['state'], REMOVED)
        self.assertEqual(r['dialog'], 0)
        self.accept([a])
        self.assertNotIn('fighter/kirby/model/body/c01/model.numdlb', self.load(A, 'mirror'))   # (kept, not used)
        self.assertEqual(len(self.blobs()), 2)
        r = self.reconcile([a, b])   # (back: nothing to fetch)
        self.assertEqual((r['mods']['Kirby Pink']['state'], r['files'], r['dialog']), (SAME, 0, 0))
        self.accept([a, b])
        self.assertIn('fighter/kirby/model/body/c01/model.numdlb', self.load(A, 'mirror'))

    def test_own_copy(self):
        data = b'shared skin' * 100
        p = self.own / 'My Pikachu' / 'fighter/pikachu/model/body/c04/model.numdlb'
        p.parent.mkdir(parents=True)
        p.write_bytes(data)
        r = self.reconcile([skin('Pikachu Ice', 'pikachu', 4, data)], own=True)
        self.assertEqual((r['files'], r['own']), (1, 1))
        res = self.accept([skin('Pikachu Ice', 'pikachu', 4, data)], own=True)
        self.assertEqual([k for _, k, ok in res['fetch']], ['own'])
        self.assertEqual(p.read_bytes(), data)   # (the player's own mod is left as it was)

    def test_gameplay_mod_that_cannot_be_had(self):
        told = {'name': 'Physics Pro', 'flags': 1, 'fighter': 'mario', 'files': []}   # (the lobby, or not shared: no files)
        r = self.reconcile([skin('Pikachu Ice', 'pikachu', 4, b'ice'), told])
        self.assertEqual((r['mods']['Physics Pro']['state'], r['missing'], r['dialog']), (UNAVAILABLE, 1, 1))

    def test_turns_of_hosting_keep_each_host_apart(self):
        common = b'the same skin' * 50
        self.accept([skin('Mario Gold', 'mario', 3, common), skin('A only', 'link', 1, b'a' * 10)], host=A, name='Alexis')
        self.accept([skin('Mario Gold (B)', 'mario', 3, common), skin('B only', 'kirby', 2, b'b' * 10)], host=B, name='Bea')
        self.assertEqual(len(self.blobs()), 3)   # (the shared skin once)
        hosts, _ = self.run_exe('hosts', self.cache)
        self.assertEqual(sorted(l.split()[2] for l in hosts.splitlines()), ['Alexis', 'Bea'])
        self.assertEqual(set(self.load(A, 'mirror').values()), {'Mario Gold', 'A only'})
        self.assertEqual(set(self.load(B, 'mirror').values()), {'Mario Gold (B)', 'B only'})

    # ---- what the game loads ----------------------------------------------------------------------
    def own_mod(self, folder, path, data=b'mine'):
        p = self.own / folder / path
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)

    def test_mirror_and_overlay(self):
        self.own_mod('My Mario', 'fighter/mario/model/body/c03/model.numdlb')
        self.own_mod('My Params', 'fighter/mario/param/fighter_param.prc')
        self.accept([skin('Host Mario', 'mario', 3, b'host mario'), skin('Host Link', 'link', 0, b'host link'), param('Host Physics', b'p')])
        mirror = self.load(A, 'mirror', own=True)
        self.assertEqual(mirror['fighter/mario/model/body/c03/model.numdlb'], 'Host Mario')
        self.assertEqual(mirror['fighter/mario/param/fighter_param.prc'], 'Host Physics')
        self.assertEqual(len(mirror), 3)   # (nothing of the player's own)
        overlay = self.load(A, 'overlay', own=True)
        self.assertEqual(overlay['fighter/mario/model/body/c03/model.numdlb'], 'My Mario')   # (the player's skin wins)
        self.assertEqual(overlay['fighter/link/model/body/c00/model.numdlb'], 'Host Link')
        self.assertEqual(overlay['fighter/mario/param/fighter_param.prc'], 'Host Physics')   # (never the player's own params)

    def test_a_missing_blob_is_left_out(self):
        self.accept([skin('Host Mario', 'mario', 3, b'host mario')])
        for b in (self.cache / '.blobs').iterdir():
            b.unlink()
        self.assertEqual(self.load(A, 'mirror'), {})

    def test_room_mods_are_normal_mod_folders(self):
        moved = skin('Link Dark', 'link', 5, b'dark' * 100, to=[-1, -1, -1, -1, -1, 6, -1, -1])
        self.accept([moved, skin('Pikachu/Ice', 'pikachu', 4, b'ice')])
        view = self.cache / 'Alexis'
        self.assertEqual((view / 'Link Dark' / 'fighter/link/model/body/c05/model.numdlb').read_bytes(), b'dark' * 100)   # (as the mod has it)
        self.assertIn('display_name = "Link Dark"', (view / 'Link Dark' / 'info.toml').read_text())
        slots = (view / 'windowsmash.toml').read_text()   # (the room's configuration: the host's colors)
        self.assertIn('[mod."Link Dark"]\nlink.c00 = "c00"', slots)
        self.assertIn('link.c05 = "c06"', slots)
        self.assertTrue((view / 'Pikachu_Ice' / 'fighter/pikachu/model/body/c04/model.numdlb').exists())
        blob = self.cache / '.blobs' / hashlib.sha256(b'ice').hexdigest()
        self.assertGreaterEqual(blob.stat().st_nlink, 2)   # (a hard link: once on disk)
        self.assertEqual(self.load(A, 'mirror')['fighter/link/model/body/c06/model.numdlb'], 'Link Dark @fighter/link/c06')   # (loaded where the host plays it)
        self.accept([skin('Link Dark', 'link', 5, b'dark' * 100)])   # (the host moved it back, dropped Pikachu)
        self.assertIn('link.c05 = "c05"', (view / 'windowsmash.toml').read_text())
        self.assertFalse((view / 'Pikachu_Ice').exists())
        self.run_exe('delete', self.cache, f'{A:016x}')
        self.assertFalse(view.exists())

    def test_new_files_through_config_json(self):
        import json
        tex = 'fighter/donkey/model/body/c01/def_monk_m_001_col.nutexb'
        config = json.dumps({'new-dir-files': {'fighter/donkey/c01': [tex]}}).encode()
        dk = skin('SSJ4 Donkey Kong', 'donkey', 1, b'model', extra=[(tex, b'new texture'), ('config.json', config)])
        self.accept([dk])
        self.assertEqual((self.cache / 'Alexis' / 'SSJ4 Donkey Kong' / 'config.json').read_bytes(), config)
        self.assertEqual(self.load(A, 'mirror')[tex], 'SSJ4 Donkey Kong @fighter/donkey/c01')   # (added to its directory, as on the host)
        moved = skin('SSJ4 Donkey Kong', 'donkey', 1, b'model', to=[-1, 3, -1, -1, -1, -1, -1, -1], extra=[(tex, b'new texture'), ('config.json', config)])
        self.accept([moved])
        self.assertEqual(self.load(A, 'mirror')[tex.replace('c01', 'c03')], 'SSJ4 Donkey Kong @fighter/donkey/c03')   # (its directory moves with it)

    # ---- forgetting -------------------------------------------------------------------------------
    def test_old_host_forgotten_and_its_files_with_it(self):
        common = b'shared by both' * 20
        now = int(time.time())
        self.accept([skin('Mario Gold', 'mario', 3, common), skin('A only', 'link', 1, b'only A' * 10)], host=A, now=now - 31 * DAY)
        self.accept([skin('Mario Gold', 'mario', 3, common)], host=B, now=now - 2 * DAY)
        old = now - 31 * DAY
        for b in (self.cache / '.blobs').iterdir():
            os.utime(b, (old, old))
        (self.cache / '.blobs' / ('ab' * 32 + '.part')).write_bytes(b'unfinished')
        fresh = self.cache / '.blobs' / ('cd' * 32)   # (a cancelled download's, an hour old: kept for the next try)
        fresh.write_bytes(b'x')
        _, log = self.run_exe('tidy', self.cache, now)
        self.assertFalse((self.cache / '.hosts' / f'{A:016x}').exists())
        self.assertTrue((self.cache / '.hosts' / f'{B:016x}' / 'host.toml').exists())
        self.assertEqual(sorted(self.blobs()), sorted([hashlib.sha256(common).hexdigest(), 'cd' * 32]))
        self.assertIn('forgotten', log)

    def test_delete_a_host(self):
        self.accept([skin('Mario Gold', 'mario', 3, b'gold')])
        out, _ = self.run_exe('delete', self.cache, f'{A:016x}')
        self.assertIn('deleted 1', out)
        self.assertEqual(self.run_exe('hosts', self.cache)[0], '')

    def test_an_edited_index_cannot_add_paths(self):
        self.accept([skin('Mario Gold', 'mario', 3, b'gold')])
        toml = self.cache / '.hosts' / f'{A:016x}' / 'host.toml'
        toml.write_text(toml.read_text() + f'file = "{"ab" * 32} 10 ../../../etc/passwd"\nfile = "{"ab" * 32} 10 skyline/plugins/x.nro"\n')
        self.assertEqual(list(self.load(A, 'mirror')), ['fighter/mario/model/body/c03/model.numdlb'])


if __name__ == '__main__':
    unittest.main()
