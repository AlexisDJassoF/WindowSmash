"""The Linux installer (install-windowsmash.sh, tools/linux/install_linux.py), the Linux build on a
Linux host (tools/linux/toolchain.py, build_game.py's step 9, recomp.py all --generate-only) and the
launchers of a folder built on Linux. What runs only on Linux is stood in for; no game files."""
from pathlib import Path
import contextlib
import io
import os
import runpy
import shutil
import subprocess
import sys
import tarfile
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools' / 'linux'))
import install_linux as il  # noqa: E402


class UpdateTests(unittest.TestCase):
    def test_source_version(self):
        sha = 'ab' * 20
        self.assertEqual(il.read_source_version(f'{sha.upper()} 2026-10-19\n'), sha)
        self.assertIsNone(il.read_source_version('$Format:%H %cs$\n'))
        self.assertIsNone(il.read_source_version(''))

    def test_decision(self):
        self.assertEqual([il.update_decision(s) for s in ('identical', 'ahead', 'behind', 'diverged', 'odd')],
                         ['current', 'update', 'newer', 'diverged', 'unknown'])

    def test_removed_paths(self):
        old = ['a.py', 'tools/gone.py', 'tools/kept.py', '../outside', '/abs', 'c:/x', 'a/../b']
        self.assertEqual(il.removed_paths(old, ['a.py', 'tools/kept.py']), ['tools/gone.py'])

    def test_online_server_merge(self):
        # (the Windows installer's cases: tests/test_installer.py, Merge-OnlineServer)
        head = '# The online lobby\n# host:port, then an optional lobby code\n'
        old, new = head + 'lobby.example:39420\n', head + 'lobby.example:39421\n'
        cases = {
            'untouched': (old, old, new, new),
            'code added': (head + 'lobby.example:39420 FRIENDS\n', old, new, head + 'lobby.example:39421 FRIENDS\n'),
            'code, CRLF': ((head + 'lobby.example:39420   FRIENDS\n').replace('\n', '\r\n'), old, new, head + 'lobby.example:39421 FRIENDS\n'),
            'own lobby': (head + 'my.server:5000\n', old, new, head + 'my.server:5000\n'),
            'off': (head + 'off\n', old, new, head + 'off\n'),
            'comment only': ('# mine\n' + head + 'lobby.example:39420\n', old, new, new),
            'old unknown': (head + 'lobby.example:39420 FRIENDS\n', None, new, head + 'lobby.example:39420 FRIENDS\n'),
            'no file': (None, old, new, new),
            'same as new': (new, old, new, new),
        }
        for k, (local, was, now, want) in cases.items():
            self.assertEqual(il.merge_online_server(local, was, now).replace('\r\n', '\n'), want, k)


class SystemTests(unittest.TestCase):
    def test_package_manager(self):
        def pm(text, have=lambda tool: True):
            return il.package_manager(il.os_release(text), have)
        self.assertEqual(pm('NAME="Ubuntu"\nID=ubuntu\nID_LIKE=debian\nPRETTY_NAME="Ubuntu 26.04 LTS"\n'), 'apt')
        self.assertEqual(pm('ID=linuxmint\nID_LIKE="ubuntu debian"\n'), 'apt')
        self.assertEqual(pm('ID=fedora\n'), 'dnf')
        self.assertEqual(pm('ID="rocky"\nID_LIKE="rhel centos fedora"\n'), 'dnf')
        self.assertEqual(pm('ID=arch\n'), 'pacman')
        self.assertEqual(pm('ID=manjaro\nID_LIKE=arch\n'), 'pacman')
        self.assertEqual(pm('ID="opensuse-tumbleweed"\nID_LIKE="opensuse suse"\n'), 'zypper')
        self.assertIsNone(pm('ID=nixos\n'))
        self.assertIsNone(pm('ID=fedora\n', have=lambda tool: tool != 'dnf'))   # (its tool missing)
        self.assertEqual(set(il.PACKAGES), set(il.INSTALL))

    def test_packages_cover_the_build(self):
        for manager, pkgs in il.PACKAGES.items():
            text = ' '.join(pkgs).lower()
            for need in ('clang', 'lld', 'llvm', 'bison', 'flex', 'sdl2', 'opus', 'vpx'):
                self.assertIn(need, text, manager)

    def test_libstdcxx_package(self):
        have = {12, 13}
        self.assertEqual(il.libstdcxx_package(['12', '13', '14'], lambda v: v in have), 'libstdc++-14-dev')
        self.assertIsNone(il.libstdcxx_package(['12', '13'], lambda v: v in have))
        self.assertIsNone(il.libstdcxx_package([], lambda v: False))

    def test_cpu_and_memory(self):
        cpu = 'processor\t: 0\nmodel name\t: AMD Ryzen 9  7950X 16-Core Processor\nflags\t\t: fpu sse2 avx avx2 bmi2\n'
        self.assertEqual(il.cpu_info(cpu), ('AMD Ryzen 9 7950X 16-Core Processor', True))
        self.assertEqual(il.cpu_info(cpu.replace(' avx2', '')), ('AMD Ryzen 9 7950X 16-Core Processor', False))
        self.assertEqual(il.cpu_info(''), ('unknown processor', None))
        self.assertAlmostEqual(il.mem_total_gb('MemTotal:       16303232 kB\nMemFree: 1 kB\n'), 15.55, places=2)
        self.assertIsNone(il.mem_total_gb(''))
        self.assertEqual(il.disk_need_gb(40, 12.5), 27.5)
        self.assertEqual(il.disk_need_gb(40, 55), 0)

    def test_folder_candidates(self):
        c = il.folder_candidates('/home/me', '/home/me/.local/share', '/home/me/.config',
                                 ['/home/me/.var/app/org.yuzu_emu.yuzu', '/home/me/.var/app/org.ryujinx.Ryujinx'])
        joined = [p.replace('\\', '/') for p in c['keys'] + c['firmware'] + c['game']]
        self.assertEqual(joined[0], '/home/me/.local/share/yuzu/keys')   # (yuzu's own folder first)
        for want in ('/home/me/.var/app/org.yuzu_emu.yuzu/data/yuzu/keys',
                     '/home/me/.local/share/eden/nand/system/Contents/registered',
                     '/home/me/.local/share/citron/nand/user/Contents/registered',
                     '/home/me/.config/Ryujinx/system', '/home/me/.config/Ryujinx/bis/user/Contents/registered',
                     '/home/me/.var/app/org.ryujinx.Ryujinx/config/Ryujinx/bis/system/Contents/registered',
                     '/home/me/.switch'):
            self.assertIn(want, joined)

    def test_saved_folders_and_checks(self):
        with tempfile.TemporaryDirectory() as tmp:
            saved = os.path.join(tmp, 'build_folders.txt')
            with open(saved, 'w') as f:
                f.write('game=/data/game\nkeys=/data/keys\nother=x\nfirmware=\n')
            self.assertEqual(il.load_saved_folders(saved), {'game': '/data/game', 'keys': '/data/keys'})
            self.assertEqual(il.load_saved_folders(os.path.join(tmp, 'none')), {})
            self.assertFalse(il.check_keys(tmp)[0])
            open(os.path.join(tmp, 'prod.keys'), 'w').close()
            self.assertEqual(il.check_keys(tmp), (True, 'prod.keys found; no title.keys (needed for games bought from the eShop)'))
            self.assertFalse(il.check_firmware(tmp)[0])
            open(os.path.join(tmp, 'a.nca'), 'w').close()
            self.assertTrue(il.check_firmware(tmp)[0])

    def test_desktop_entry(self):
        self.assertEqual(il.desktop_quote('bash'), 'bash')
        self.assertEqual(il.desktop_quote('/home/me/My Games/$x"`\\/play-ssbu.sh'), '"/home/me/My Games/\\$x\\"\\`\\\\/play-ssbu.sh"')
        text = il.desktop_entry('/home/me/Win Smash')
        self.assertIn('Exec=bash "/home/me/Win Smash/play-ssbu.sh"\nPath=/home/me/Win Smash\n', text)
        self.assertIn('Terminal=true\n', text)
        self.assertTrue(text.startswith('[Desktop Entry]\nType=Application\n'))


class PartsTests(unittest.TestCase):
    def test_parts_match_the_toolchain(self):
        """Where the parts go is where tools/linux/toolchain.py and build_vkd3d.py look."""
        wheres = {p[4] for p in il.PARTS}
        self.assertTrue({'dxvk-native', 'dxvk-headers/include/native', 'dxvk-headers/include/native/directx'} <= wheres)
        self.assertTrue(any(w.startswith('src/vkd3d-') for w in wheres))
        self.assertTrue(any(w.startswith('src/SPIRV-Headers-') for w in wheres))
        for p in il.PARTS:
            self.assertRegex(p[2], r'^[0-9a-f]{64}$')
            self.assertTrue(p[1].startswith('https://'))
        # a part inside another's folder comes after it (it is unpacked again after that one)
        order = [p[4] for p in il.PARTS]
        self.assertLess(order.index('dxvk-headers/include/native'), order.index('dxvk-headers/include/native/directx'))

    def test_strip_member(self):
        self.assertEqual(il.strip_member('dxvk-3.1.1/include/native/windows/a.h', 'dxvk-3.1.1/include/native/'), 'windows/a.h')
        self.assertIsNone(il.strip_member('dxvk-3.1.1/src/x.cpp', 'dxvk-3.1.1/include/native/'))
        self.assertIsNone(il.strip_member('dxvk-3.1.1/include/native/', 'dxvk-3.1.1/include/native/'))
        self.assertEqual(il.strip_member('./usr/lib/x.so', ''), 'usr/lib/x.so')
        self.assertEqual(il.strip_member('./.hidden', ''), '.hidden')

    def test_extract_part(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = os.path.join(tmp, 'a.tar.gz')
            with tarfile.open(archive, 'w:gz') as t:
                for name, data in (('top/inc/x.h', b'x'), ('top/inc/sub/y.h', b'y'), ('top/other.c', b'z')):
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    t.addfile(info, io.BytesIO(data))
                link = tarfile.TarInfo('top/inc/z.h')
                link.type, link.linkname = tarfile.LNKTYPE, 'top/inc/x.h'
                t.addfile(link)
            dest = os.path.join(tmp, 'out', 'inc')
            os.makedirs(dest)
            open(os.path.join(dest, 'stale.h'), 'w').close()   # (made afresh)
            il.extract_part(archive, dest, 'top/inc/')
            got = sorted(os.path.relpath(os.path.join(r, f), dest).replace('\\', '/') for r, _d, fs in os.walk(dest) for f in fs)
            self.assertEqual(got, ['sub/y.h', 'x.h', 'z.h'])
            with open(os.path.join(dest, 'z.h'), 'rb') as f:
                self.assertEqual(f.read(), b'x')


class ToolchainTests(unittest.TestCase):
    def test_pick_llvm_bin(self):
        import toolchain
        have = {('/usr/lib/llvm-18/bin', t) for t in toolchain.LLVM_TOOLS} | \
               {('/usr/lib/llvm-21/bin', 'clang'), ('/usr/bin', 'clang'), ('/usr/bin', 'ld.lld'), ('/usr/bin', 'llvm-ar'), ('/usr/bin', 'llvm-nm')}
        dirs = sorted(['/usr/lib/llvm-18/bin', '/usr/lib/llvm-21/bin', '/usr/lib/llvm-9/bin'], key=toolchain.llvm_version, reverse=True)
        self.assertEqual(dirs, ['/usr/lib/llvm-21/bin', '/usr/lib/llvm-18/bin', '/usr/lib/llvm-9/bin'])
        has = lambda d, t: (d, t) in have   # noqa: E731
        # Ubuntu: llvm-21 has only clang (no lld-21); /usr/bin is llvm-18's: the newest folder with all four
        self.assertEqual(toolchain.pick_llvm_bin(dirs + ['/usr/bin'], has, lambda d: toolchain.llvm_version(d) or 18),
                         '/usr/lib/llvm-18/bin')
        self.assertEqual(toolchain.pick_llvm_bin(['/usr/bin'], has), '/usr/bin')
        self.assertIsNone(toolchain.pick_llvm_bin(['/nowhere'], lambda d, t: False))
        self.assertEqual(toolchain.llvm_version('/usr/lib64/llvm17/bin'), 17)
        self.assertEqual(toolchain.llvm_version('/usr/lib/llvm14/bin'), 14)
        # Arch, Fedora: an older compatibility LLVM next to the system's newest in /usr/bin
        arch = {(d, t) for d in ('/usr/lib/llvm14/bin', '/usr/bin') for t in toolchain.LLVM_TOOLS}
        self.assertEqual(toolchain.pick_llvm_bin(['/usr/lib/llvm14/bin', '/usr/bin'], lambda d, t: (d, t) in arch,
                                                 lambda d: toolchain.llvm_version(d) or 21), '/usr/bin')


class BuildOnLinuxTests(unittest.TestCase):
    """build_game.py's step 9 on Linux: the C generated, vkd3d-shader built once, the game compiled
    and linked for Linux."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='linux-build-test-')
        self.addCleanup(self.temp.cleanup)
        top = Path(self.temp.name)
        dump = top / 'port' / 'dump'
        (dump / 'exefs').mkdir(parents=True)
        (dump / 'modules').mkdir()
        for n in ('main', 'main.npdm', 'rtld', 'sdk', 'subsdk0'):
            (dump / 'exefs' / n).write_bytes(b'')
        self.vkd3d = top / 'build-vkd3d'
        self.calls = []
        self.ns = runpy.run_path(str(ROOT / 'tools/build_game.py'), run_name='build_game_linux')['build'].__globals__
        fake_tc = types.SimpleNamespace(VKD3D_BUILD=str(self.vkd3d))
        self.ns.update(
            LINUX=True, ROOT=str(top / 'port'), DUMP=str(dump), EXEFS=str(dump / 'exefs'), FONTS=str(dump / 'fonts'),
            MODULES=str(dump / 'modules'), SAVED=str(dump / 'build_folders.txt'), SOURCE=str(dump / 'game_source.txt'),
            EXE=str(top / 'port' / 'build-linux' / 'ssbu'), log=lambda msg='': None, check_tools=lambda: None,
            install_packages=lambda: None, ask_folder=lambda key, given, saved: str(top),
            romfs_cfg_ok=lambda cfg: True, missing_fonts=lambda folder: [], check_same_game=lambda rec: None,
            find_program=lambda game, keys: ('p', 0x01006A800016E000, {}, {'contents': []}),
            installed_ram_gb=lambda: 8.0, linux_toolchain=lambda: fake_tc,
            run=lambda args, failure: self.calls.append([str(a) for a in args]))
        (dump / 'modules' / 'aoc.cfg').write_text('')
        self.args = types.SimpleNamespace(game=None, keys=None, firmware=None, jobs=None, prepare_only=False)

    def tools_run(self):
        return [next((Path(a).name for a in c if a.endswith('.py')), c[0]) for c in self.calls]

    def test_step_9(self):
        self.ns['build'](self.args)
        self.assertEqual(self.tools_run(), ['arc.py', 'recomp.py', 'build_vkd3d.py', 'build_linux.py'])
        recomp, vkd3d, linux = self.calls[1:]
        self.assertIn('--generate-only', recomp)
        self.assertEqual(recomp[recomp.index('--jobs') + 1], '4')   # (8 GB of RAM)
        self.assertEqual(vkd3d[-1], 'linux')
        self.assertEqual(linux[linux.index('--port') + 1], self.ns['ROOT'])
        self.assertEqual(linux[linux.index('--out') + 1], 'build-linux')

    def test_vkd3d_once(self):
        (self.vkd3d / 'linux').mkdir(parents=True)
        (self.vkd3d / 'linux' / 'libvkd3d-shader.a').write_bytes(b'')
        self.ns['build'](self.args)
        self.assertEqual(self.tools_run(), ['arc.py', 'recomp.py', 'build_linux.py'])


class GenerateOnlyTests(unittest.TestCase):
    def test_no_compile_no_link(self):
        """recomp.py all --generate-only: every module's C, stamped; nothing compiled or linked."""
        g = runpy.run_path(str(ROOT / 'tools/recomp.py'), run_name='recomp_generate')['build_all'].__globals__
        done = []
        with tempfile.TemporaryDirectory() as tmp:
            exefs = os.path.join(tmp, 'exefs')
            os.makedirs(exefs)
            for n in ('main', 'sdk'):
                open(os.path.join(exefs, n), 'wb').close()
            stamps = []
            g.update(
                TREE=tmp, NRO_DIR=os.path.join(tmp, 'nro'),
                module_paths=lambda e: {'main': os.path.join(e, 'main'), 'sdk': os.path.join(e, 'sdk')},
                check=lambda e: 0, tools_fingerprint=lambda: 'fp', image=lambda nso, img: done.append(('image', nso)),
                Module=lambda nso: types.SimpleNamespace(build_id='b' * 64), MODULES={'main': 1, 'sdk': 2},
                build=lambda nso, target, jobs, per_file: done.append(('build', os.path.basename(target))),
                compile_dir=lambda target, jobs: done.append(('compile', target)) or True,
                link=lambda dirs, exe: done.append(('link', exe)) or True,
                modcache=types.SimpleNamespace(build_info=lambda t: None, find_built=lambda *x: None,
                                               write_stamp=lambda t, *x: stamps.append(os.path.basename(t))))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertTrue(g['build_all'](exefs, os.path.join(tmp, 'build'), 4, False))
            self.assertEqual([d for d in done if d[0] in ('compile', 'link')], [])
            self.assertEqual([d[1] for d in done if d[0] == 'build'], ['main', 'sdk'])
            self.assertEqual(stamps, ['main', 'sdk'])
            done.clear()
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertTrue(g['build_all'](exefs, os.path.join(tmp, 'build'), 4))   # (the Windows build: both)
            self.assertEqual([d[0] for d in done if d[0] in ('compile', 'link')], ['compile', 'compile', 'link'])


def find_bash():
    """A bash that runs scripts here (on Windows, Git's; not WSL's launcher), or None."""
    candidates = [shutil.which('bash')]
    if os.name == 'nt':
        git = shutil.which('git')
        if git:
            candidates.insert(0, str(Path(git).resolve().parent.parent / 'bin' / 'bash.exe'))
    for c in candidates:
        if c and os.path.isfile(c) and 'system32' not in c.lower():
            try:
                if subprocess.run([c, '-c', 'readlink -f .'], capture_output=True, timeout=30).returncode == 0:
                    return c
            except (OSError, subprocess.TimeoutExpired):
                pass
    return None


BASH = find_bash()


@unittest.skipUnless(BASH, 'needs bash')
class LauncherTests(unittest.TestCase):
    """The launchers of a folder built on Linux (play-ssbu.sh and the rest) run the package's
    launchers for it: build-linux/ssbu with dump/modules, from the folder, after their checks."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='launcher-test-')
        self.addCleanup(self.temp.cleanup)
        self.top = Path(self.temp.name) / 'Win Smash'
        top = self.top
        for name in ('play-ssbu.sh', 'launchers/play-ssbu-offline.sh'):   # (one at the top, one in launchers/)
            (top / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ROOT / name, top / name)
        shutil.copytree(ROOT / 'tools' / 'linux' / 'bundle', top / 'tools' / 'linux' / 'bundle')
        (top / 'build-linux').mkdir()
        (top / 'dump' / 'modules').mkdir(parents=True)
        data = top / 'game.nca'
        data.write_bytes(b'data')
        (top / 'dump' / 'modules' / 'romfs.cfg').write_text(f'{data.as_posix()}\n1234\n', newline='\n')
        (top / 'online_server.txt').write_text('off\n', newline='\n')
        # the game: says how it was started; ldd and ldconfig: the libraries are there
        (top / 'build-linux' / 'ssbu').write_text('#!/usr/bin/env bash\necho "GAME $PWD | $*"\n', newline='\n')
        fake = Path(self.temp.name) / 'bin'
        fake.mkdir()
        (fake / 'ldd').write_text('#!/usr/bin/env bash\necho "libSDL2-2.0.so.0 => /usr/lib/libSDL2-2.0.so.0"\n', newline='\n')
        (fake / 'ldconfig').write_text('#!/usr/bin/env bash\necho "libvulkan.so.1 (libc6,x86-64) => /usr/lib/libvulkan.so.1"\n', newline='\n')
        self.env = {**os.environ, 'VK_ICD_FILENAMES': 'icd.json'}
        self.path = fake

    def run_launcher(self, name):
        script = f'export PATH="$(cygpath -u "{self.path}" 2>/dev/null || echo "{self.path}"):$PATH"; cd /; bash "$1"'
        return subprocess.run([BASH, '-c', script, 'x', str(self.top / name)], capture_output=True, text=True,
                              env=self.env, timeout=60)

    def test_offline(self):
        r = self.run_launcher('launchers/play-ssbu-offline.sh')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        game = [line for line in r.stdout.splitlines() if line.startswith('GAME ')]
        self.assertEqual(len(game), 1, r.stdout)
        self.assertTrue(game[0].endswith('| dump/modules --status 30'), game[0])
        self.assertIn('Win Smash |', game[0])   # (started from the folder above launchers/)
        self.assertTrue((self.top / 'build' / 'play.log').exists())

    def test_online_off_plays_offline(self):
        r = self.run_launcher('play-ssbu.sh')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('Playing offline (no lobby in online_server.txt).', r.stdout)

    def test_data_gone(self):
        (self.top / 'game.nca').unlink()
        r = self.run_launcher('launchers/play-ssbu-offline.sh')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('The game data is not where it was when the game was built', r.stdout)
        self.assertNotIn('GAME ', r.stdout)

    def test_not_built(self):
        (self.top / 'build-linux' / 'ssbu').unlink()
        r = self.run_launcher('launchers/play-ssbu-offline.sh')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('The game is not built yet (build-linux/ssbu): run  bash install-windowsmash.sh', r.stdout)


@unittest.skipUnless(BASH, 'needs bash')
class ScriptSyntaxTests(unittest.TestCase):
    def test_bash_syntax(self):
        scripts = [ROOT / n for n in il.LAUNCHERS] + sorted((ROOT / 'tools' / 'linux' / 'bundle').rglob('*.sh'))
        for s in scripts:
            r = subprocess.run([BASH, '-n', str(s)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, f'{s.name}: {r.stderr}')
            self.assertNotIn(b'\r\n', s.read_bytes(), s.name)


if __name__ == '__main__':
    unittest.main()
