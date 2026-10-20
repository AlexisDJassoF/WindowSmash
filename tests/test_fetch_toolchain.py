"""tools/linux/fetch_toolchain.py: reading the archive's indexes, extracting safely and refusing a
download that is not the expected file. Offline: nothing is fetched."""
from pathlib import Path
import io
import os
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools/linux'))
import fetch_toolchain  # noqa: E402

RELEASE = """Origin: Ubuntu
Suite: jammy-updates
MD5Sum:
 fa7b0343f37e1f1794ecd78744c361a3          3874796 main/binary-amd64/Packages.xz
SHA256:
 ffc71d80f7b53269a6477370d9159de3cb2af837d2262810572d3f3974971a8d          3874796 main/binary-amd64/Packages.xz
 0000000000000000000000000000000000000000000000000000000000000000              103 main/binary-amd64/Release
Acquire-By-Hash: yes
"""

PACKAGES = """Package: libc6
Version: 2.35-0ubuntu3.15
Filename: pool/main/g/glibc/libc6_2.35-0ubuntu3.15_amd64.deb
SHA256: aaaa

Package: libopus-dev
Version: 1.3.1-0.1build2
Filename: pool/main/o/opus/libopus-dev_1.3.1-0.1build2_amd64.deb
SHA256: bbbb

Package: libc6
Version: 2.35-0ubuntu3.1
Filename: pool/main/g/glibc/libc6_2.35-0ubuntu3.1_amd64.deb
SHA256: cccc
"""


def make_tar(path, entries):
    """entries: (name, data bytes | ('symlink', target))."""
    with tarfile.open(path, 'w:gz') as tar:
        for name, data in entries:
            info = tarfile.TarInfo(name)
            if isinstance(data, tuple):
                info.type, info.linkname = tarfile.SYMTYPE, data[1]
                tar.addfile(info)
            else:
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))


class IndexTests(unittest.TestCase):
    def test_release_sha256_from_sha256_section(self):
        self.assertEqual(fetch_toolchain.release_sha256(RELEASE, 'main/binary-amd64/Packages.xz'),
                         'ffc71d80f7b53269a6477370d9159de3cb2af837d2262810572d3f3974971a8d')

    def test_release_sha256_missing(self):
        with self.assertRaises(fetch_toolchain.Failed):
            fetch_toolchain.release_sha256(RELEASE, 'universe/binary-amd64/Packages.xz')

    def test_parse_packages_first_stanza_wins(self):
        found = fetch_toolchain.parse_packages(PACKAGES)
        self.assertEqual(found['libc6']['Version'], '2.35-0ubuntu3.15')
        self.assertEqual(found['libc6']['SHA256'], 'aaaa')
        self.assertEqual(found['libopus-dev']['Filename'], 'pool/main/o/opus/libopus-dev_1.3.1-0.1build2_amd64.deb')


class ExtractTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='fetch-test-')
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)

    def test_inner_folder_only(self):
        archive = self.folder / 'a.tar.gz'
        make_tar(archive, [('pkg-1.0/include/a.h', b'a'), ('pkg-1.0/README', b'r'), ('other/b.h', b'b')])
        fetch_toolchain.extract(str(archive), 'pkg-1.0/include', str(self.folder / 'out'))
        self.assertEqual(sorted(p.name for p in (self.folder / 'out').iterdir()), ['a.h'])

    def test_unsafe_path_refused(self):
        archive = self.folder / 'bad.tar.gz'
        make_tar(archive, [('../escape.txt', b'x')])
        with self.assertRaises(fetch_toolchain.Failed):
            fetch_toolchain.extract(str(archive), '', str(self.folder / 'out'))
        self.assertFalse((self.folder / 'escape.txt').exists())

    def test_absolute_links_point_inside(self):
        root = self.folder / 'sysroot'
        (root / 'lib/x86_64-linux-gnu').mkdir(parents=True)
        (root / 'usr/lib/x86_64-linux-gnu').mkdir(parents=True)
        (root / 'lib/x86_64-linux-gnu/libm.so.6').write_text('m')
        os.symlink('/lib/x86_64-linux-gnu/libm.so.6', root / 'usr/lib/x86_64-linux-gnu/libm.so')
        fetch_toolchain.keep_links_inside(str(root))
        link = root / 'usr/lib/x86_64-linux-gnu/libm.so'
        self.assertFalse(os.readlink(link).startswith('/'))
        self.assertEqual(link.read_text(), 'm')


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='fetch-test-')
        self.addCleanup(self.temp.cleanup)
        self.saved = fetch_toolchain.TOOLCHAIN, fetch_toolchain.DOWNLOADS
        fetch_toolchain.TOOLCHAIN = self.temp.name
        fetch_toolchain.DOWNLOADS = os.path.join(self.temp.name, 'downloads')

    def tearDown(self):
        fetch_toolchain.TOOLCHAIN, fetch_toolchain.DOWNLOADS = self.saved

    def test_same_inputs_left_alone_and_failure_keeps_old(self):
        def fill(staging):
            Path(staging, 'file').write_text('one')
        self.assertTrue(fetch_toolchain.install('part', 'v1', fill))
        self.assertFalse(fetch_toolchain.install('part', 'v1', fill))

        def broken(staging):
            Path(staging, 'file').write_text('half')
            raise fetch_toolchain.Failed('stopped')
        with self.assertRaises(fetch_toolchain.Failed):
            fetch_toolchain.install('part', 'v2', broken)
        self.assertEqual(Path(self.temp.name, 'part', 'file').read_text(), 'one')
        self.assertEqual([p for p in os.listdir(self.temp.name) if p.endswith('.partial')], [])

    def test_kept_download_with_other_hash_is_fetched_again(self):
        os.makedirs(fetch_toolchain.DOWNLOADS)
        kept = Path(fetch_toolchain.DOWNLOADS, 'x.deb')
        kept.write_bytes(b'not it')
        # (file:// so nothing leaves the machine; its content is not the expected one either)
        source = Path(self.temp.name, 'source.deb')
        source.write_bytes(b'also not it')
        with self.assertRaises(fetch_toolchain.Failed):
            fetch_toolchain.download(source.as_uri(), '0' * 64, 'x.deb')
        self.assertFalse(Path(fetch_toolchain.DOWNLOADS, 'x.deb.part').exists())


if __name__ == '__main__':
    unittest.main()
