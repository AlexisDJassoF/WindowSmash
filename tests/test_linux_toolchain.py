"""tools/linux/toolchain.py: finding the LLVM tools on a Linux host. No compiler needed: the module
is loaded with SSBU_LLVM_BIN naming a folder of empty stand-ins, and find_linux_llvm is given a
made-up PATH."""
from pathlib import Path
import fnmatch
import importlib.util
import os
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ('clang', 'ld.lld', 'llvm-ar', 'llvm-nm')


def load_toolchain(llvm_bin):
    saved = os.environ.get('SSBU_LLVM_BIN')
    os.environ['SSBU_LLVM_BIN'] = llvm_bin
    try:
        spec = importlib.util.spec_from_file_location('toolchain_under_test', ROOT / 'tools/linux/toolchain.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        if sys.platform.startswith('linux'):
            module.tools()   # (found now, while SSBU_LLVM_BIN names the stand-ins)
        return module
    finally:
        if saved is None:
            del os.environ['SSBU_LLVM_BIN']
        else:
            os.environ['SSBU_LLVM_BIN'] = saved


class FakeSystem:
    """A file system of executables, for which, isfile and glob."""
    def __init__(self, files):
        self.files = set(files)

    def which(self, name):
        for path in sorted(self.files):
            if path.startswith('/usr/bin/') and os.path.basename(path) == name:
                return path
        return None

    def isfile(self, path):
        return path in self.files

    def glob(self, pattern):
        """Files matching pattern, or with a pattern ending in /bin, the folders holding files."""
        paths = {os.path.dirname(f) for f in self.files} if pattern.endswith('/bin') else self.files
        return sorted(p for p in paths if fnmatch.fnmatch(p, pattern))

    def find(self, env):
        return TOOLCHAIN.find_linux_llvm(env=env, which=self.which, isfile=self.isfile, find=self.glob)


TEMP = tempfile.TemporaryDirectory(prefix='toolchain-test-')
for _name in TOOLS:
    Path(TEMP.name, _name).write_text('')
TOOLCHAIN = load_toolchain(TEMP.name)


@unittest.skipUnless(sys.platform.startswith('linux'), 'the Linux host branch')
class LinuxHostTests(unittest.TestCase):
    def test_module_uses_named_folder(self):
        self.assertEqual(TOOLCHAIN.CLANG, os.path.join(TEMP.name, 'clang'))
        self.assertEqual(TOOLCHAIN.LLVM_AR, os.path.join(TEMP.name, 'llvm-ar'))
        self.assertEqual(TOOLCHAIN.LLVM_NM, os.path.join(TEMP.name, 'llvm-nm'))
        self.assertEqual(TOOLCHAIN.LINKER, ['--ld-path=' + os.path.join(TEMP.name, 'ld.lld')])
        self.assertFalse(TOOLCHAIN.CLANG.endswith('.exe'))

    def test_named_folder_missing_a_tool(self):
        fake = FakeSystem({'/opt/llvm/bin/clang', '/opt/llvm/bin/llvm-ar', '/opt/llvm/bin/llvm-nm'})
        with self.assertRaises(SystemExit) as caught:
            fake.find({'SSBU_LLVM_BIN': '/opt/llvm/bin', 'PATH': '/usr/bin'})
        self.assertIn('ld.lld', str(caught.exception))

    def test_unsuffixed_on_path(self):
        fake = FakeSystem({f'/usr/bin/{t}' for t in TOOLS} | {f'/usr/bin/{t}-20' for t in TOOLS})
        self.assertEqual(fake.find({'PATH': '/usr/bin'}), {t: f'/usr/bin/{t}' for t in TOOLS})

    def test_newest_llvm_folder(self):
        fake = FakeSystem({f'/usr/lib/llvm-{v}/bin/{t}' for v in (18, 20) for t in TOOLS} | {'/usr/lib/llvm-21/bin/clang'})
        self.assertEqual(fake.find({'PATH': '/usr/bin'}), {t: f'/usr/lib/llvm-20/bin/{t}' for t in TOOLS})

    def test_suffixed_only(self):
        fake = FakeSystem({f'/usr/bin/{t}-20' for t in TOOLS} | {f'/usr/bin/{t}-18' for t in TOOLS} | {'/usr/bin/clang-21'})
        self.assertEqual(fake.find({'PATH': '/usr/bin'}), {t: f'/usr/bin/{t}-20' for t in TOOLS})

    def test_missing_lld_names_package(self):
        fake = FakeSystem({'/usr/bin/clang', '/usr/bin/llvm-ar', '/usr/bin/llvm-nm'})
        with self.assertRaises(SystemExit) as caught:
            fake.find({'PATH': '/usr/bin'})
        self.assertIn('sudo apt install lld', str(caught.exception))

    def test_nothing_installed_names_all_packages(self):
        fake = FakeSystem(set())
        with self.assertRaises(SystemExit) as caught:
            fake.find({'PATH': '/usr/bin'})
        self.assertIn('sudo apt install clang lld llvm', str(caught.exception))


class WindowsBranchTests(unittest.TestCase):
    def test_windows_branch_unchanged(self):
        source = (ROOT / 'tools/linux/toolchain.py').read_text()
        windows = source[source.index('llvm = _find_llvm()'):]
        self.assertIn('"CLANG": os.path.join(llvm, "clang.exe")', windows)
        self.assertIn('"LLVM_AR": os.path.join(llvm, "llvm-ar.exe")', windows)
        self.assertIn('"LINKER": ["-fuse-ld=lld"]', windows)

    def test_layout_needs_no_compiler(self):
        """Reading the layout (package_linux.py) does not look for the LLVM tools."""
        saved = os.environ.pop('SSBU_LLVM_BIN', None)
        try:
            with mock.patch('shutil.which', return_value=None), mock.patch('glob.glob', return_value=[]):
                spec = importlib.util.spec_from_file_location('toolchain_layout_only', ROOT / 'tools/linux/toolchain.py')
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
                self.assertTrue(module.DXVK_LIBS)
        finally:
            if saved is not None:
                os.environ['SSBU_LLVM_BIN'] = saved


if __name__ == '__main__':
    unittest.main()
