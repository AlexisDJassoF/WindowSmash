"""The installer's decisions (tools/install_windowsmash.ps1): its pure functions, run in Windows
PowerShell with made-up inputs. No downloads, no installs, no game files."""
from pathlib import Path
import json
import os
import subprocess
import unittest

ROOT = Path(__file__).resolve().parent.parent
PS = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
SCRIPT = ROOT / 'tools/install_windowsmash.ps1'


def ps(expression):
    """The JSON of a PowerShell expression evaluated with the installer's functions loaded."""
    command = f". '{SCRIPT}'; {expression} | ConvertTo-Json -Compress -Depth 6"
    r = subprocess.run([str(PS), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', command],
                       capture_output=True, text=True)
    if r.returncode or not r.stdout.strip():
        raise AssertionError(r.stdout + r.stderr)
    return json.loads(r.stdout)


SDL_RELEASES = json.dumps([
    {"tag_name": "release-3.5.0", "prerelease": True, "draft": False,
     "assets": [{"name": "SDL3-devel-3.5.0-VC.zip", "browser_download_url": "https://x/pre.zip", "digest": "sha256:" + "a" * 64}]},
    {"tag_name": "release-3.4.20", "prerelease": False, "draft": False,
     "assets": [{"name": "SDL3-devel-3.4.20-mingw.zip", "browser_download_url": "https://x/mingw.zip"}]},
    {"tag_name": "release-3.4.16", "prerelease": False, "draft": False,
     "assets": [{"name": "SDL3-devel-3.4.16-VC.zip", "browser_download_url": "https://x/16.zip", "digest": "sha256:" + "b" * 64}]},
    {"tag_name": "release-3.4.18", "prerelease": False, "draft": False,
     "assets": [{"name": "SDL3-3.4.18.tar.gz", "browser_download_url": "https://x/src.tar.gz"},
                {"name": "SDL3-devel-3.4.18-VC.zip", "browser_download_url": "https://x/18.zip", "digest": "sha256:" + "C" * 64}]},
    {"tag_name": "release-2.32.0", "prerelease": False, "draft": False,
     "assets": [{"name": "SDL2-devel-2.32.0-VC.zip", "browser_download_url": "https://x/sdl2.zip"}]},
])


@unittest.skipUnless(PS.exists(), 'Windows PowerShell only')
class InstallerDecisionTests(unittest.TestCase):
    def test_python_embed_url_newest_of_the_minor(self):
        listing = 'href="3.14.6/" href="3.9.1/" href="3.14.8/" href="3.15.0/" href="3.14.10/"'
        got = ps(f"@{{ pick = (Get-PythonEmbedUrl '{listing}' '3.14' '3.14.0'); none = (Get-PythonEmbedUrl '' '3.14' '3.14.8') }}")
        self.assertEqual(got['pick']['Version'], '3.14.10')
        self.assertEqual(got['pick']['Url'], 'https://www.python.org/ftp/python/3.14.10/python-3.14.10-embed-amd64.zip')
        self.assertEqual(got['none']['Version'], '3.14.8')
        self.assertTrue(got['none']['Url'].endswith('/3.14.8/python-3.14.8-embed-amd64.zip'))

    def test_sdl3_asset_newest_release_with_a_vc_zip(self):
        releases = SDL_RELEASES.replace("'", "''")
        got = ps(f"$r = (Select-Sdl3Asset ('{releases}' | ConvertFrom-Json)); @{{ v = $r.Version.ToString(); url = $r.Url; sha = $r.Sha256; name = $r.Name }}")
        self.assertEqual(got['v'], '3.4.18')   # not the prerelease 3.5.0, not 3.4.20 without a VC zip
        self.assertEqual(got['url'], 'https://x/18.zip')
        self.assertEqual(got['sha'], 'c' * 64)
        self.assertEqual(got['name'], 'SDL3-devel-3.4.18-VC.zip')

    def test_sdl3_asset_without_digest_or_releases(self):
        releases = json.dumps([{"tag_name": "release-3.4.4", "prerelease": False, "draft": False,
                                "assets": [{"name": "SDL3-devel-3.4.4-VC.zip", "browser_download_url": "https://x/4.zip"}]}])
        got = ps(f"$r = (Select-Sdl3Asset ('{releases}' | ConvertFrom-Json)); @{{ v = $r.Version.ToString(); sha = $r.Sha256; none = (Select-Sdl3Asset @()) }}")
        self.assertEqual(got['v'], '3.4.4')
        self.assertIsNone(got['sha'])
        self.assertIsNone(got['none'])

    def test_visual_studio_arguments(self):
        got = ps("@{ install = (Get-VsBuildToolsArguments 'Microsoft.VisualStudio.BuildTools'); "
                 "bt = (Get-VsModifyArguments 'C:\\VS\\BuildTools' 'Microsoft.VisualStudio.Product.BuildTools'); "
                 "community = (Get-VsModifyArguments 'C:\\VS\\Community' 'Microsoft.VisualStudio.Product.Community') }")
        install = got['install']
        self.assertEqual(install[:3], ['install', '--id', 'Microsoft.VisualStudio.BuildTools'])
        self.assertIn('--accept-package-agreements', install)
        self.assertIn('--accept-source-agreements', install)
        override = install[install.index('--override') + 1]
        for part in ('--passive', '--wait', '--norestart', '--add Microsoft.VisualStudio.Workload.VCTools',
                     '--add Microsoft.VisualStudio.Component.VC.Llvm.Clang', '--includeRecommended'):
            self.assertIn(part, override)
        self.assertEqual(got['bt'][:3], ['modify', '--installPath', 'C:\\VS\\BuildTools'])
        self.assertIn('Microsoft.VisualStudio.Workload.VCTools', got['bt'])
        self.assertIn('Microsoft.VisualStudio.Workload.NativeDesktop', got['community'])
        self.assertIn('Microsoft.VisualStudio.Component.VC.Llvm.Clang', got['community'])
        self.assertIn('--passive', got['community'])

    def test_folder_candidates_and_resolution(self):
        got = ps("$c = Get-FolderCandidates 'C:\\Users\\me\\AppData\\Roaming' 'C:\\Users\\me'; "
                 "$have = @('C:\\Users\\me\\AppData\\Roaming\\Ryujinx\\system', 'C:\\Users\\me\\.switch'); "
                 "@{ keys0 = $c.Keys[0]; firmware0 = $c.Firmware[0]; game0 = $c.Game[0]; "
                 "ryu = (Resolve-FirstExisting $c.Keys { param($p) $have -contains $p }); "
                 "none = (Resolve-FirstExisting $c.Game { param($p) $false }); "
                 "ryugame = ($c.Game -contains 'C:\\Users\\me\\AppData\\Roaming\\Ryujinx\\bis\\user\\Contents\\registered'); "
                 "ryufw = ($c.Firmware -contains 'C:\\Users\\me\\AppData\\Roaming\\Ryujinx\\bis\\system\\Contents\\registered'); "
                 "switch = ($c.Keys -contains 'C:\\Users\\me\\.switch') }")
        self.assertEqual(got['keys0'], 'C:\\Users\\me\\AppData\\Roaming\\yuzu\\keys')
        self.assertEqual(got['firmware0'], 'C:\\Users\\me\\AppData\\Roaming\\yuzu\\nand\\system\\Contents\\registered')
        self.assertEqual(got['game0'], 'C:\\Users\\me\\AppData\\Roaming\\yuzu\\nand\\user\\Contents\\registered')
        self.assertEqual(got['ryu'], 'C:\\Users\\me\\AppData\\Roaming\\Ryujinx\\system')   # the first that exists
        self.assertIsNone(got['none'])
        self.assertTrue(got['ryugame'] and got['ryufw'] and got['switch'])

    def test_avx2_by_processor_name(self):
        cases = {
            'Intel(R) Core(TM) i5-4590 CPU @ 3.30GHz': True,
            'Intel(R) Core(TM) i7-12700K': True,
            'Intel(R) Core(TM) i7-3770 CPU @ 3.40GHz': False,
            'Intel(R) Core(TM) Ultra 7 155H': True,
            'AMD Ryzen 5 3600 6-Core Processor': True,
            'AMD EPYC 7302P': True,
            'Intel(R) Core(TM)2 Duo CPU E8400': False,
            'Intel(R) Celeron(R) N4000 CPU': None,
            'Intel(R) Xeon(R) Gold 6148': None,
        }
        names = '; '.join(f"'{n}' = (Test-Avx2ByName '{n}')" for n in cases)
        got = ps('@{ ' + names + ' }')
        for name, want in cases.items():
            self.assertEqual(got[name], want, name)

    def test_small_decisions(self):
        got = ps("@{ fw = (Get-FirewallRuleCommand 'C:\\WindowSmash\\build\\ssbu.exe' 'WindowSmash (ssbu.exe)'); "
                 "v1 = (Test-PythonVersionText 'Python 3.14.6'); v2 = (Test-PythonVersionText '3.10.0'); "
                 "v3 = (Test-PythonVersionText '3.9.13'); v4 = (Test-PythonVersionText 'Python was not found'); "
                 "v5 = (Test-PythonVersionText '2.7.18'); "
                 "d1 = (Get-DiskNeedGb 60 25); d2 = (Get-DiskNeedGb 60 70) }")
        self.assertIn('New-NetFirewallRule', got['fw'])
        self.assertIn("-Program 'C:\\WindowSmash\\build\\ssbu.exe'", got['fw'])
        self.assertIn("-DisplayName 'WindowSmash (ssbu.exe)'", got['fw'])
        self.assertIn('-Direction Inbound', got['fw'])
        self.assertEqual([got['v1'], got['v2'], got['v3'], got['v4'], got['v5']], [True, True, False, False, False])
        self.assertEqual(got['d1'], 35)
        self.assertEqual(got['d2'], 0)


if __name__ == '__main__':
    unittest.main()
