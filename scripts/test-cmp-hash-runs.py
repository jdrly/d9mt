#!/usr/bin/env python3
"""Checks of scripts/cmp-hash-runs.py on planted hashes.txt files. No Wine, no GPU."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('cmp-hash-runs.py')

# Lines as scripts/run-hash-tests.sh writes them (the first three are a real run's).
BASE = '''\
code_alpha rc=0 fails=0 ab71104dcbddc043 ab71104dcbddc043 ab71104dcbddc043 ab71104dcbddc043 ab71104dcbddc043
code_attach rc=0 fails=0 3ea8d4e87904a7a3 3ea8d4e87904a7a3 3ea8d4e87904a7a3 3ea8d4e87904a7a3 3ea8d4e87904a7a3
code_blend rc=0 fails=0 9d8d42e3a595c803 9d8d42e3a595c803 9d8d42e3a595c803 9d8d42e3a595c803 9d8d42e3a595c803
code_mtlock rc=0 fails=0 1111111111111111 2222222222222222 3333333333333333 4444444444444444 5555555555555555
code_readback rc=0 fails=0 aaaaaaaaaaaaaaaa
'''


class Compare(unittest.TestCase):
    def run_cmp(self, base, head, *args):
        with tempfile.TemporaryDirectory() as d:
            b, h = Path(d) / 'base.txt', Path(d) / 'head.txt'
            b.write_text(base)
            h.write_text(head)
            return subprocess.run([sys.executable, '-I', str(SCRIPT), str(b), str(h), *args],
                                  text=True, capture_output=True)

    def test_equal_runs_pass(self):
        r = self.run_cmp(BASE, BASE)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn('common 5 programs; frames differ: none; not allowed to: none', r.stdout)

    def test_one_frame_differs(self):
        head = BASE.replace('3ea8d4e87904a7a3 3ea8d4e87904a7a3\n', '3ea8d4e87904a7a3 0000000000000000\n')
        r = self.run_cmp(BASE, head)
        self.assertEqual(r.returncode, 1)
        self.assertIn('frames differ: code_attach; not allowed to: code_attach', r.stdout)
        self.assertEqual(self.run_cmp(BASE, head, '--allow', 'code_attach').returncode, 0)

    def test_a_missing_frame_is_a_difference(self):
        head = BASE.replace('code_blend rc=0 fails=0 9d8d42e3a595c803 ', 'code_blend rc=0 fails=0 ')
        r = self.run_cmp(BASE, head)
        self.assertEqual(r.returncode, 1)
        self.assertIn('not allowed to: code_blend', r.stdout)

    def test_mtlock_may_differ(self):
        head = BASE.replace('1111111111111111', '9999999999999999')
        r = self.run_cmp(BASE, head)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn('frames differ: code_mtlock; not allowed to: none', r.stdout)

    def test_a_program_gone_from_head_fails(self):
        head = ''.join(l for l in BASE.splitlines(True) if not l.startswith('code_blend'))
        r = self.run_cmp(BASE, head)
        self.assertEqual(r.returncode, 1)
        self.assertIn('only in base: code_blend', r.stdout)

    def test_a_new_program_must_be_named(self):
        head = BASE + 'code_new rc=0 fails=0 bbbbbbbbbbbbbbbb\n'
        r = self.run_cmp(BASE, head)
        self.assertEqual(r.returncode, 1)
        self.assertIn('only in head: code_new (not named in --new: code_new)', r.stdout)
        self.assertEqual(self.run_cmp(BASE, head, '--new', 'code_new').returncode, 0)

    def test_head_not_clean(self):
        for old, new, shown in (
                ('code_alpha rc=0 fails=0', 'code_alpha rc=137 fails=0', 'code_alpha(rc=137,fails=0,5 frames)'),
                ('code_alpha rc=0 fails=0', 'code_alpha rc=0 fails=2', 'code_alpha(rc=0,fails=2,5 frames)')):
            head = BASE.replace(old, new)
            r = self.run_cmp(BASE, head)
            self.assertEqual(r.returncode, 1, r.stdout)
            self.assertIn(shown, r.stdout)
            self.assertIn('exit status or FAIL lines changed: code_alpha: rc=0 fails=0 -> ', r.stdout)
            self.assertEqual(self.run_cmp(BASE, head, '--allow-notclean', 'code_alpha').returncode, 0)

    def test_no_frames_at_all_is_not_clean(self):
        both = BASE.replace('code_readback rc=0 fails=0 aaaaaaaaaaaaaaaa', 'code_readback rc=0 fails=0')
        r = self.run_cmp(both, both)
        self.assertEqual(r.returncode, 1)
        self.assertIn('code_readback(rc=0,fails=0,0 frames)', r.stdout)

    def test_not_clean_on_both_sides_still_needs_the_flag(self):
        both = BASE.replace('code_alpha rc=0 fails=0', 'code_alpha rc=77 fails=0')
        r = self.run_cmp(both, both)
        self.assertEqual(r.returncode, 1)
        self.assertIn('exit status or FAIL lines changed: none', r.stdout)
        self.assertEqual(self.run_cmp(both, both, '--allow-notclean', 'code_alpha').returncode, 0)

    def test_nothing_in_common_fails(self):
        r = self.run_cmp('code_a rc=0 fails=0 aa\n', 'code_b rc=0 fails=0 aa\n', '--new', 'code_b')
        self.assertEqual(r.returncode, 1)
        self.assertIn('nothing was compared', r.stdout)

    def test_other_lines_are_ignored(self):
        r = self.run_cmp('hash.sh: 5 programs\n' + BASE, BASE + 'STATS variant=default programs=5\n')
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_usage(self):
        self.assertNotEqual(subprocess.run([sys.executable, '-I', str(SCRIPT), 'one'], capture_output=True).returncode, 0)
        self.assertNotEqual(subprocess.run([sys.executable, '-I', str(SCRIPT), 'a', 'b', '--nope'], capture_output=True).returncode, 0)


if __name__ == '__main__':
    unittest.main()
