from pathlib import Path
import hashlib
import os
import runpy
import sys
import struct
import subprocess
import tempfile
import unittest

WORK = Path(__file__).resolve().parent
PS = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
SCRIPT = WORK.parent / 'tools/Unlock-All-Fighters.ps1'
ORIGINAL = runpy.run_path(str(WORK.parent / 'tools/unlock_all.py'), run_name='unlock_reference')


class UnlockTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='unlock-test-', dir=WORK)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.save = self.root / 'save/user/save_data/system_data.bin'
        self.save.parent.mkdir(parents=True)
        data = bytearray(ORIGINAL['SAVE_SIZE'])
        struct.pack_into('<I', data, 0, len(data))
        for n in ORIGINAL['STARTERS']:
            data[ORIGINAL['FLAGS'] + n * 8] = 1
        # A locked rematch candidate with an existing encounter order.
        data[ORIGINAL['FLAGS'] + 10 * 8 + 1] = 3
        data[ORIGINAL['FLAGS'] + 10 * 8 + 4] = 1
        self.before = bytes(data)
        self.save.write_bytes(data)

    def invoke(self, *args, mock_processes=True):
        # Only the process listing is mocked: the user's real running game stays untouched.
        command = 'function Get-Process { @() }; ' if mock_processes else ''
        command += f"& '{str(SCRIPT).replace(chr(39), chr(39)*2)}' -SavePath '{self.save}' " + ' '.join(args)
        return subprocess.run([str(PS), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', command], capture_output=True, text=True)

    def test_dry_run_preserves_save(self):
        result = self.invoke('-DryRun')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Fighters to unlock: 66', result.stdout)
        self.assertEqual(self.save.read_bytes(), self.before)
        self.assertFalse((self.root / 'save_backups').exists())

    def test_unlock_matches_reference_and_preserves_other_bytes(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = self.save.read_bytes()
        flags, records = ORIGINAL['FLAGS'], ORIGINAL['RECORDS']
        changed = {i for i, (a, b) in enumerate(zip(self.before, data)) if a != b}
        allowed = set()
        for n in range(len(ORIGINAL['FIGHTERS'])):
            allowed.update(range(flags + 8 * n, flags + 8 * n + 5))
            allowed.update(range(records + 8 * n, records + 8 * n + 8))
            self.assertEqual(data[flags + 8 * n], 1)
        self.assertLessEqual(changed, allowed)
        self.assertEqual(data[flags + 10 * 8 + 1:flags + 10 * 8 + 4], b'\0\0\0')
        self.assertEqual(data[flags + 10 * 8 + 4], 1)
        for n in set(range(len(ORIGINAL['FIGHTERS']))) - ORIGINAL['STARTERS']:
            record = struct.unpack_from('<Q', data, records + 8 * n)[0]
            self.assertTrue(record & (1 << 63))
            self.assertEqual((record >> 48) & 255, 3)
            self.assertGreaterEqual(record & 4095, 2026)
        backups = list((self.root / 'save_backups').iterdir())
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), self.before)
        reference = self.root / 'reference/save/user/save_data/system_data.bin'
        reference.parent.mkdir(parents=True)
        reference.write_bytes(self.before)
        comparison = subprocess.run([sys.executable, str(WORK.parent / 'tools/unlock_all.py'), '--save', str(reference)], capture_output=True, text=True)
        self.assertEqual(comparison.returncode, 0, comparison.stdout + comparison.stderr)
        self.assertEqual(reference.read_bytes()[flags:flags + 8 * 74], data[flags:flags + 8 * 74])
        again = self.invoke()
        self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
        self.assertEqual(self.save.read_bytes(), data)
        self.assertEqual(len(list((self.root / 'save_backups').iterdir())), 1)

    def test_invalid_layout_rejected(self):
        data = bytearray(self.before)
        data[ORIGINAL['FLAGS']] = 0
        self.save.write_bytes(data)
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.save.read_bytes(), data)
        self.assertFalse((self.root / 'save_backups').exists())

    def test_running_game_guard_preserves_save(self):
        # A deterministic process-list mock with one item tests the refusal path.
        command = f"function Get-Process {{ [pscustomobject]@{{Id=1}} }}; & '{SCRIPT}' -SavePath '{self.save}'"
        result = subprocess.run([str(PS), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', command], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Close every SSBU', result.stdout)
        self.assertEqual(self.save.read_bytes(), self.before)


if __name__ == '__main__':
    unittest.main(verbosity=2)
