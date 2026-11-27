"""The room's manifest (runtime/mod_manifest.c, openspec change netplay-mod-sync): what a host announces
of its mods, and what who joins takes from a manifest - valid, cut short and made to harm. Built with
tests/mod_manifest_test.c; the manifests made to harm come from the encoder here, written from the
format in runtime/mod_manifest.h."""
from pathlib import Path
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
CC = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')

SKIN = ['fighter/palutena/model/body/c03/model.numdlb', 'ui/replace/chara/chara_3/chara_3_palutena_03.bntx']
PARAM = ['fighter/mario/param/fighter_param.prc']


def mod_id(files):
    """files: (path, sha256 bytes) - the id mani_mod_id makes"""
    h = hashlib.sha256()
    for path, sha in sorted(files):
        h.update(path.encode() + b'\0' + sha)
    v = int.from_bytes(h.digest()[:8], 'big')
    return v or 1


def text(s, n=1):
    b = s.encode()
    return (struct.pack('>B', len(b)) if n == 1 else struct.pack('>H', len(b))) + b


def encode(mods, host_id=0x1122334455667788, host='Alexis', lobby=0, nfiles=None, nmods=None):
    """mods: dicts of name, flags, files [(path, size, sha)], and optionally id, fighter, to, url"""
    files = [f for m in mods for f in m.get('files', [])]
    out = b'SNMF' + bytes([2]) + struct.pack('>Q', host_id) + host.encode().ljust(32, b'\0')
    out += struct.pack('>BBH', lobby, len(mods) if nmods is None else nmods, len(files) if nfiles is None else nfiles)
    for m in mods:
        fl = m.get('files', [])
        mid = m.get('id', mod_id([(p, s) for p, _, s in fl]) if fl else 7)
        out += struct.pack('>QB', mid, m['flags']) + text(m['name']) + text(m.get('version', '1.0')) + text(m.get('fighter', 'palutena'))
        out += struct.pack('8b', *m.get('to', [-1] * 8)) + struct.pack('>H', len(fl))
        if m['flags'] & 4:
            out += text(m['url'], 2) + m.get('url_sha', b'\x11' * 32)
    for p, size, sha in files:
        out += text(p) + struct.pack('>I', size) + sha
    return out


def sha(data):
    return hashlib.sha256(data).digest()


def visual(name='Skin', paths=SKIN, **kw):
    return {'name': name, 'flags': 2, 'files': [(p, 16, sha(p.encode())) for p in paths], **kw}


@unittest.skipUnless(sys.platform.startswith('linux') and CC, 'Linux and a C compiler')
class ManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='mod-manifest-test-')
        cls.exe = Path(cls.temp.name) / 'mod_manifest_test'
        r = subprocess.run([CC, '-O1', '-w', '-D_GNU_SOURCE', '-I', str(ROOT / 'runtime'), str(ROOT / 'tests/mod_manifest_test.c'),
                            str(ROOT / 'runtime/mod_manifest.c'), str(ROOT / 'runtime/mod_sha256.c'), str(ROOT / 'runtime/mod_scan.c'),
                            '-o', str(cls.exe)], capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(f'compiling the test failed:\n{r.stdout}{r.stderr}')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        d = tempfile.TemporaryDirectory(prefix='mod-manifest-run-')
        self.addCleanup(d.cleanup)
        self.dir = Path(d.name)
        self.mods = self.dir / 'mods'
        self.mods.mkdir()

    def mod(self, folder, files, info=None, data=None):
        for f in files:
            p = self.mods / folder / f
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data if data is not None else (folder + f).encode())
        if info:
            (self.mods / folder / 'info.toml').write_text(info)

    def run_exe(self, *args, **env):
        base = {k: v for k, v in os.environ.items() if not k.startswith('SSBU_')}
        r = subprocess.run([str(self.exe), *map(str, args)], capture_output=True, text=True, env={**base, **env}, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout, r.stderr

    def read(self, out):
        lines = out.splitlines()
        if lines[0].startswith('refused'):
            return {'refused': lines[0][8:]}
        h = lines[0].split()
        res = {'host_id': h[1], 'host': h[2], 'lobby': int(h[3]), 'nmods': int(h[4]), 'nfiles': int(h[5]), 'bytes': int(h[6]), 'mods': []}
        for line in lines[1:]:
            w = line.split('\t') if '\t' in line else line.split(' ')
            if w[0] == 'mod':
                name, version, fighter = w[3].split('|')
                res['mods'].append(dict(id=w[1], flags=int(w[2]), name=name, version=version, fighter=fighter,
                                        to=[int(c) for c in w[4].split(',')], nfiles=int(w[5]), bytes=int(w[6]),
                                        why=w[7].replace(' url', ''), url=line.endswith(' url'), files=[]))
            elif w[0] == 'file':
                res['mods'][-1]['files'].append((w[1], int(w[2]), w[3]))
            elif w[0] == 'hash':
                res['hash'] = w[1]
        return res

    def build(self, *opts, **env):
        out, log = self.run_exe('build', self.mods, self.dir / 'manifest.bin', *opts, **env)
        return self.read(out), log

    def parse(self, data, *opts):
        f = self.dir / 'in.bin'
        f.write_bytes(data)
        return self.read(self.run_exe('parse', f, *opts)[0])

    # ---- the host's manifest ------------------------------------------------------------------
    def test_skin_and_param_mod_in_a_private_room(self):
        self.mod('Pit Skin', SKIN, info='display_name = "Palutena Ice"\nversion = "2.1"\n')
        self.mod('Physics', PARAM)
        m, _ = self.build()
        self.assertEqual((m['host_id'], m['host'], m['lobby'], m['nmods']), ('1122334455667788', 'Alexis', 0, 2))
        phys, skin = m['mods']   # (alphabetical: Physics, Pit Skin)
        self.assertEqual((skin['name'], skin['version'], skin['fighter'], skin['flags']), ('Palutena Ice', '2.1', 'palutena', 2))
        self.assertEqual(phys['flags'], 3)   # (gameplay, with its files)
        want = sorted((p, sha((('Pit Skin' + p).encode()))) for p in SKIN)
        self.assertEqual(sorted((p, bytes.fromhex(h)) for p, _, h in skin['files']), want)
        self.assertEqual(skin['id'], f'{mod_id(want):016x}')
        self.assertEqual(m['nfiles'], 3)
        # the encoding is the one described: the parser reads it back as it was
        again = self.parse((self.dir / 'manifest.bin').read_bytes())
        self.assertEqual([(x['id'], x['flags'], x['files']) for x in again['mods']], [(x['id'], x['flags'], x['files']) for x in m['mods']])
        self.assertTrue(all(x['why'] == 'kept' for x in again['mods']))

    def test_lobby_lists_gameplay_mods_without_files(self):
        self.mod('Pit Skin', SKIN)
        self.mod('Physics', PARAM)
        m, _ = self.build('lobby')
        phys, skin = m['mods']
        self.assertEqual((phys['flags'], phys['nfiles'], skin['flags'], skin['nfiles']), (1, 0, 2, 2))
        self.assertEqual(m['lobby'], 1)
        # its id is still its files': who has it can tell
        self.assertEqual(phys['id'], f'{mod_id([(PARAM[0], sha(("Physics" + PARAM[0]).encode()))]):016x}')

    def test_share_off_tells_only_gameplay_mods(self):
        self.mod('Pit Skin', SKIN)
        self.mod('Physics', PARAM)
        m, _ = self.build('off')
        self.assertEqual([(x['name'], x['flags'], x['nfiles']) for x in m['mods']], [('Physics', 1, 0)])

    def test_no_share_and_position(self):
        self.mod('Pit Skin', SKIN)
        self.mod('Link Skin', ['fighter/link/model/body/c05/model.numdlb'])
        self.mod('Physics', PARAM)
        (self.mods / 'windowsmash.toml').write_text('[mod."Link Skin"]\nshare = false\n\n[mod."Pit Skin"]\npalutena.c03 = "c06"\n\n'
                                                    '[mod."Physics"]\nshare = false\n')
        m, _ = self.build()
        names = {x['name']: x for x in m['mods']}
        self.assertNotIn('Link Skin', names)                                   # (a visual mod not shared: not told)
        self.assertEqual((names['Physics']['flags'], names['Physics']['nfiles']), (1, 0))   # (a gameplay one: told, no files)
        self.assertEqual(names['Pit Skin']['to'], [-1, -1, -1, 6, -1, -1, -1, -1])

    def test_disabled_and_downloaded_mods_are_not_told(self):
        self.mod('.Off Skin', SKIN)
        self.mod('.netplay/blobs', ['fighter/mario/model/body/c00/x.numdlb'])
        m, _ = self.build()
        self.assertEqual(m['nmods'], 0)

    def test_no_mods(self):
        m, _ = self.build()
        self.assertEqual((m['nmods'], m['nfiles'], m['bytes']), (0, 0, 0))

    def test_code_and_files_outside_data_arc_are_not_given(self):
        self.mod('Pit Skin', SKIN + ['fighter/palutena/plugin.nro', 'skyline/plugins/libx.nro', 'stream;/sound/bgm/x.nus3audio'])
        m, log = self.build()
        self.assertEqual(sorted(p for p, _, _ in m['mods'][0]['files']), sorted(SKIN))
        self.assertIn('[mods] rooms: Pit Skin:', log)

    def test_a_file_over_the_limit_leaves_the_mod_out(self):
        self.mod('Pit Skin', SKIN)
        self.mod('Big Skin', ['fighter/mario/model/body/c00/model.numdlb'])
        with open(self.mods / 'Big Skin/fighter/mario/model/body/c00/model.numdlb', 'r+b') as f:
            f.truncate(200 << 20)   # (sparse: 200 MB)
        m, log = self.build()
        self.assertEqual([x['name'] for x in m['mods']], ['Pit Skin'])
        self.assertIn('[mods] rooms: Big Skin:', log)
        self.assertIn('over 128 MB', log)

    def test_url(self):
        h = 'ab' * 32
        self.mod('Pit Skin', SKIN, info=f'url = "https://example.org/pit.zip"\nurl_sha256 = "{h}"\n')
        self.mod('Mario Skin', ['fighter/mario/model/body/c00/m.numdlb'], info='url = "https://example.org/m.zip"\n')   # (no hash: not used)
        m, _ = self.build()
        self.assertEqual({x['name']: x['url'] for x in m['mods']}, {'Pit Skin': True, 'Mario Skin': False})

    def test_hashes_are_kept_and_made_again_when_a_file_changes(self):
        self.mod('Pit Skin', SKIN)
        cache = self.dir / 'hashes.txt'
        first, _ = self.build(HASH_CACHE=str(cache))
        self.assertEqual(len([l for l in cache.read_text().splitlines() if not l.startswith('#')]), 2)
        # a forged cache entry with the same size and time is believed (it is the cache): proves it is read
        lines = cache.read_text().splitlines()
        forged = [('00' * 32 + l[64:]) if SKIN[0].split('/')[-1] in l else l for l in lines]
        cache.write_text('\n'.join(forged) + '\n')
        second, _ = self.build(HASH_CACHE=str(cache))
        self.assertIn('0' * 64, [h for _, _, h in second['mods'][0]['files']])
        # the file changed (another size): hashed again
        (self.mods / 'Pit Skin' / SKIN[0]).write_bytes(b'something else entirely')
        third, _ = self.build(HASH_CACHE=str(cache))
        self.assertIn(sha(b'something else entirely').hex(), [h for _, _, h in third['mods'][0]['files']])
        self.assertNotIn('0' * 64, cache.read_text())

    def test_config_json_is_given_and_hidden_files_are_not(self):
        self.mod('DK', ['fighter/donkey/model/body/c01/model.numdlb', 'fighter/donkey/model/body/c01/new_tex.nutexb',
                        'fighter/.DS_Store', 'fighter/donkey/._model.numdlb'])
        (self.mods / 'DK' / 'Config.json').write_text('{"new-dir-files": {"fighter/donkey/c01": ["fighter/donkey/model/body/c01/new_tex.nutexb"]}}')
        m, _ = self.build()
        paths = sorted(p for p, _, _ in m['mods'][0]['files'])
        self.assertEqual(paths, ['config.json', 'fighter/donkey/model/body/c01/model.numdlb', 'fighter/donkey/model/body/c01/new_tex.nutexb'])
        self.assertEqual(m['mods'][0]['flags'], 2)   # (config.json is no gameplay file)
        again = self.parse((self.dir / 'manifest.bin').read_bytes())
        self.assertEqual(again['mods'][0]['why'], 'kept')

    # ---- reading one --------------------------------------------------------------------------
    def test_valid(self):
        m = self.parse(encode([visual(), dict(name='Physics', flags=3, fighter='mario', files=[(PARAM[0], 99, sha(b'p'))])]))
        self.assertEqual([(x['name'], x['flags'], x['why']) for x in m['mods']], [('Skin', 2, 'kept'), ('Physics', 3, 'kept')])
        self.assertEqual(m['bytes'], 16 * 2 + 99)

    def test_cut_short_anywhere_is_refused(self):
        data = encode([visual(), visual('Two', ['fighter/mario/model/body/c01/a.numdlb'], url='https://x.org/a.zip', flags=6)])
        for n in range(len(data)):
            self.assertIn('refused', self.parse(data[:n]), n)
        self.assertIn('refused', self.parse(data + b'\0'))   # (and nothing past its end)

    def test_counts_that_do_not_add_up(self):
        self.assertIn('refused', self.parse(encode([visual()], nfiles=3)))
        self.assertIn('refused', self.parse(encode([visual()] * 129)))
        self.assertIn('refused', self.parse(encode([visual()], host_id=0)))
        self.assertIn('refused', self.parse(b'SNMF\x03' + encode([visual()])[5:]))   # (another version)

    def test_paths_that_could_harm_drop_their_mod(self):
        bad = ['../save/system.dat', '/etc/passwd', 'fighter/mario/../../../x', 'skyline/plugins/a.nro', 'fighter/mario/a.nro',
               'fighter/mario/a.lua', 'stream:/sound/bgm/a.nus3audio', 'Fighter/Mario/a.numdlb', 'fighter//mario', 'fighter/mario/',
               'fighter\\mario\\a', 'top.bin', 'prebuilt;/nro/release/a.nro', 'fighter/mario/./a']
        for p in bad:
            m = self.parse(encode([visual(), visual('Evil', [p])]))
            self.assertEqual(m['mods'][0]['why'], 'kept', p)
            self.assertTrue(m['mods'][1]['why'].startswith('dropped:'), p)

    def test_other_bad_mods_are_dropped_alone(self):
        good = visual()
        cases = [dict(visual('Huge'), files=[('fighter/mario/model/body/c00/a.numdlb', (128 << 20) + 1, sha(b'a'))]),
                 dict(visual('Empty'), files=[('fighter/mario/model/body/c00/a.numdlb', 0, sha(b'a'))]),
                 dict(visual('Wrong id'), id=12345),
                 dict(visual('Twice'), files=[('fighter/mario/model/body/c00/a.numdlb', 1, sha(b'a'))] * 2),
                 dict(visual('Color'), to=[9, -1, -1, -1, -1, -1, -1, -1]),
                 dict(visual('Fighter'), fighter='../x'),
                 dict(visual('Flags'), flags=2 | 64)]
        for bad in cases:
            m = self.parse(encode([good, bad]))
            self.assertEqual(m['mods'][0]['why'], 'kept', bad['name'])
            self.assertTrue(m['mods'][1]['why'].startswith('dropped:'), (bad['name'], m['mods'][1]['why']))

    def test_room_total(self):
        big = [dict(visual(f'Big{i}'), files=[(f'fighter/mario/model/body/c0{i % 8}/a{i}.numdlb', 120 << 20, sha(str(i).encode()))]) for i in range(35)]
        m = self.parse(encode(big))
        self.assertEqual([x['why'] == 'kept' for x in m['mods']], [True] * 34 + [False])   # (34 x 120 MB fit in 4 GB, the 35th does not)

    def test_the_paths_decide_the_kind(self):
        m = self.parse(encode([dict(visual('Sneaky', PARAM), fighter='mario')]))
        self.assertEqual(m['mods'][0]['flags'] & 1, 1)   # (it said visual; its file is a parameter file)

    def test_lobby_never_takes_gameplay_files(self):
        data = encode([visual(), dict(name='Physics', flags=3, fighter='mario', files=[(PARAM[0], 99, sha(b'p'))])])
        m = self.parse(data, 'lobby')
        self.assertEqual([(x['flags'], x['nfiles']) for x in m['mods']], [(2, 2), (1, 0)])
        self.assertEqual(m['bytes'], 32)
        m = self.parse(encode([dict(name='Physics', flags=3, fighter='mario', files=[(PARAM[0], 99, sha(b'p'))])], lobby=1))   # (it says so itself)
        self.assertEqual(m['mods'][0]['flags'], 1)

    def test_urls(self):
        ok = dict(visual(), flags=6, url='https://example.org/a.zip')
        self.assertTrue(self.parse(encode([ok]))['mods'][0]['url'])
        for u in ('http://example.org/a.zip', 'file:///etc/passwd', 'https://a b'):
            m = self.parse(encode([dict(visual(), flags=6, url=u)]))
            self.assertEqual((m['mods'][0]['url'], m['mods'][0]['why']), (False, 'kept'), u)   # (from the host, then)

    def test_allowed_paths(self):
        out, _ = self.run_exe('path', 'fighter/mario/model/body/c00/model.numdlb', 'ui/replace/chara/chara_0/chara_0_mario_00.bntx',
                              'sound/bank/fighter_voice/vc_mario.nus3audio', 'effect/fighter/mario/ef_mario.eff', 'stage/battlefield/normal/a.nutexb',
                              'fighter/mario/a.nro', 'mods/x/a.bin')
        self.assertEqual(out.split(), ['1', '1', '1', '1', '1', '0', '0'])


if __name__ == '__main__':
    unittest.main()
