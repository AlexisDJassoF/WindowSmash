"""tools/recomp.py gen-all: the C of every module, nothing compiled or linked, and a later all
still compiling what gen-all left. The generator itself is stood in for: gen-all calls the same
build() as all, so what is tested here is what it calls and what it leaves."""
from pathlib import Path
import os
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import modcache  # noqa: E402
import recomp  # noqa: E402


class FakeModule:
    def __init__(self, path):
        self.build_id = 'b' * 32 + os.path.basename(path)


class GenAllTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='gen-all-test-')
        self.addCleanup(self.temp.cleanup)
        self.tree = Path(self.temp.name)
        (self.tree / 'dump/exefs').mkdir(parents=True)
        (self.tree / 'dump/modules').mkdir(parents=True)
        for name in ('main', 'sdk'):
            (self.tree / 'dump/exefs' / name).write_bytes(b'nso')
        self.built = []

        def fake_build(nso, target, jobs, per_file):
            self.built.append(os.path.basename(nso))
            os.makedirs(target, exist_ok=True)
            Path(target, 'g000.c').write_text('/* C */\n')
            Path(target, 'g_module.c').write_text('/* table */\n')
            Path(target, 'manifest.tsv').write_text('g000.c\t10\t10\n')

        self.patches = [
            mock.patch.object(recomp, 'TREE', str(self.tree)),
            mock.patch.object(recomp, 'MODULES', {'main': 0x7100000000, 'sdk': 0x7109000000}),
            mock.patch.object(recomp, 'module_paths',
                              lambda exefs: {n: os.path.join(exefs, n) for n in ('main', 'sdk')}),
            mock.patch.object(recomp, 'check', lambda exefs: None),
            mock.patch.object(recomp, 'tools_fingerprint', lambda: 'fp'),
            mock.patch.object(recomp, 'image', lambda nso, out: Path(out).write_bytes(b'img')),
            mock.patch.object(recomp, 'Module', FakeModule),
            mock.patch.object(recomp, 'build', fake_build),
            mock.patch.object(modcache, 'find_built', lambda *a: None),
        ]
        for p in self.patches:
            p.start()
            self.addCleanup(p.stop)
        self.out = str(self.tree / 'build')

    def test_generates_without_compiling_or_linking(self):
        compiled, linked = mock.Mock(return_value=True), mock.Mock(return_value=True)
        with mock.patch.object(recomp, 'compile_dir', compiled), mock.patch.object(recomp, 'link', linked), \
                mock.patch.dict(sys.modules, {'cc': None}):   # (importing cc would fail)
            self.assertTrue(recomp.build_all(str(self.tree / 'dump/exefs'), self.out, 4, compile=False))
        compiled.assert_not_called()
        linked.assert_not_called()
        self.assertEqual(sorted(self.built), ['main', 'sdk'])
        for name in ('main', 'sdk'):
            folder = os.path.join(self.out, name)
            self.assertTrue(os.path.isfile(os.path.join(folder, 'manifest.tsv')))
            self.assertEqual(modcache.build_info(folder)[2:], (name, 'fp'))
            self.assertFalse([n for n in os.listdir(folder) if n.endswith('.obj')])

    def test_second_run_reuses_the_c(self):
        exefs = str(self.tree / 'dump/exefs')
        recomp.build_all(exefs, self.out, 4, compile=False)
        self.built.clear()
        recomp.build_all(exefs, self.out, 4, compile=False)
        self.assertEqual(self.built, [])

    def test_all_after_gen_all_still_compiles(self):
        """The stamp gen-all writes makes all skip generating, never compiling: compile_dir
        goes by each object's time, and gen-all leaves none."""
        exefs = str(self.tree / 'dump/exefs')
        recomp.build_all(exefs, self.out, 4, compile=False)
        self.built.clear()
        jobs = []
        fake_cc = types.SimpleNamespace(compile_each=lambda todo, n: jobs.extend(todo) or [])
        with mock.patch.dict(sys.modules, {'cc': fake_cc}), mock.patch.object(recomp, 'link', mock.Mock(return_value=True)):
            self.assertTrue(recomp.build_all(exefs, self.out, 4))
        self.assertEqual(self.built, [])
        compiled = sorted(os.path.relpath(src, self.out) for src, _obj, _opt in jobs)
        self.assertEqual(compiled, [os.path.join(m, f) for m in ('main', 'sdk') for f in ('g000.c', 'g_module.c')])


if __name__ == '__main__':
    unittest.main()
