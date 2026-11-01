#!/usr/bin/env python3
"""Synthetic reporting checks. No benchmarks, Wine or display access."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('perf-compare.py')
spec = importlib.util.spec_from_file_location('compare', SCRIPT)
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


class Reports(unittest.TestCase):
    def run_report(self, text, *args):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'results.txt'
            path.write_text(text)
            return subprocess.run([sys.executable, str(SCRIPT), *args, str(path)],
                                  text=True, capture_output=True)

    def pairs(self, key='avg_ms', value=110, count=8, base=100):
        return ''.join(f'round={i} v=base RESULT {key}={base}\n'
                       f'round={i} v=change RESULT {key}={value}\n'
                       for i in range(count))

    def test_directions_and_polarity(self):
        for metric in ('avg_ms', 'p99', 'fps', 'low1_fps', 'frame_low1_fps'):
            for value in (90, 110):
                with self.subTest(metric=metric, value=value):
                    r = self.run_report(self.pairs(metric, value), '--metric', metric,
                                        '--diagnostic-band', '1')
                    self.assertEqual(r.returncode, 0, r.stderr)
                    slower = (value > 100) != (metric in compare.HIGHER_IS_BETTER)
                    self.assertIn(f"DIAGNOSTIC {'slower' if slower else 'faster'} >1%", r.stdout)
                    self.assertIn('slower 8/8' if slower else 'faster 8/8', r.stdout)
                    self.assertIn('magnitude 10.0000', r.stdout)
                    self.assertIn('conditional median interval:', r.stdout)
                    self.assertIn('guard: pending;', r.stdout)

    def test_optional_band_and_boundaries(self):
        self.assertNotIn('DIAGNOSTIC', self.run_report(self.pairs()).stdout)
        for effect in (-.0025, .0025):
            self.assertFalse(compare.over_band(effect, .25))
        for effect in (-.002501, .002501):
            self.assertTrue(compare.over_band(effect, .25))
        for value in (99.75, 100.25):
            r = self.run_report(self.pairs(value=value), '--diagnostic-band', '.25')
            self.assertNotIn('DIAGNOSTIC', r.stdout)
        for band in ('-1', 'nan', 'inf'):
            self.assertNotEqual(self.run_report(self.pairs(), '--diagnostic-band', band).returncode, 0)

    def test_intervals(self):
        self.assertEqual(compare.median_interval([1] * 5), (None, None))
        self.assertEqual(compare.median_interval(list(range(6))), (0, 5))
        self.assertEqual(compare.median_interval(list(range(20))), (5, 14))
        self.assertEqual(compare.median_interval([-2] * 8), (-2, -2))
        r = self.run_report(self.pairs(count=1))
        self.assertIn('interval: unbounded', r.stdout)

    def test_equal_and_zero_reference(self):
        r = self.run_report(self.pairs(value=100))
        self.assertIn('faster 0/8, slower 0/8, equal 8/8', r.stdout)
        r = self.run_report(self.pairs(value=1, base=0))
        self.assertEqual(r.returncode, 0)
        self.assertIn('(n/a)', r.stdout)
        self.assertIn('guard: pending;', r.stdout)

    def test_invalid_missing_and_unpaired(self):
        cases = [('DIRTY round=8 v=change RESULT avg_ms=110', '1 DIRTY', 3),
                 ('round=8 v=change RESULT FAIL timeout', '1 FAIL', 1),
                 ('round=8 v=change RESULT fps=20', '1 missing', 3)]
        cases += [(f'round=8 v=change RESULT avg_ms={v}', '1 invalid', 3)
                  for v in ('nan', 'inf', '-1', '1.2.3')]
        for line, reason, code in cases:
            with self.subTest(line=line):
                r = self.run_report(self.pairs() + line + '\n')
                self.assertEqual(r.returncode, code, r.stderr)
                self.assertIn(reason, r.stdout)
                self.assertIn('guard: pending;', r.stdout)
        r = self.run_report(self.pairs() + 'round=8 v=base avg_ms=100\n')
        self.assertEqual(r.returncode, 3)
        self.assertIn('incomplete pairing', r.stdout)
        r = self.run_report('round=1 v=base avg_ms=1\nround=2 v=change avg_ms=2\n')
        self.assertEqual(r.returncode, 3)
        self.assertIn('invalid comparison', r.stdout)
        for text, reason in (('', '1 missing'),
                             ('round=1 v=base avg_ms=nan\n', '1 invalid'),
                             ('round=1 v=base avg_ms=1\n', 'need results for at least two variants'),
                             (self.pairs() + self.pairs(), 'a second sample')):
            r = self.run_report(text)
            self.assertEqual(r.returncode, 3)
            self.assertIn(reason, r.stderr)
        r = self.run_report('round=1 v=base RESULT FAIL timeout\n')
        self.assertEqual(r.returncode, 1)
        self.assertIn('1 FAIL', r.stderr)
        r = self.run_report('round=1 v=base RESULT FAIL timeout\nround=1 v=change avg_ms=1\n', '--ref', 'base')
        self.assertEqual(r.returncode, 1)
        self.assertIn('no results for the reference base', r.stderr)

    def test_paired_band_and_relative_interval(self):
        # The two marginal medians are equal, but the paired median is +1.
        text = ''.join(f'round={i} v=base avg_ms={b}\nround={i} v=change avg_ms={v}\n'
                       for i, (b, v) in enumerate([(1, 2), (2, 3), (3, 1)] * 3))
        r = self.run_report(text, '--diagnostic-band', '25')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('DIAGNOSTIC slower >25%', r.stdout)
        self.assertIn('relative -100.000% .. +50.000%', r.stdout)
        self.assertIn('magnitude 1.0000 (50.000%)', r.stdout)

    def test_controls_cannot_supply_calibration(self):
        text = self.pairs(value=100) + ''.join(f'round={i} v=copy avg_ms=100\n' for i in range(8))
        r = self.run_report(text, '--control', 'copy')
        self.assertEqual(r.returncode, 0)
        self.assertIn('inside observed control range; equivalence unverified', r.stdout)
        self.assertIn('no accepted metric/schedule/condition calibration', r.stdout)
        self.assertNotIn('~ noise', r.stdout)
        for control in ('base', 'absent'):
            self.assertNotEqual(self.run_report(text, '--control', control).returncode, 0)
        self.assertIn('calibration: none', r.stdout)
        self.assertIn('[control]', r.stdout)
        self.assertNotIn('inside observed control range', r.stdout.split('[control]')[1])
        r = self.run_report(self.pairs() + 'round=99 v=copy avg_ms=100\n', '--control', 'copy')
        self.assertEqual(r.returncode, 3)
        self.assertIn('control has no paired results', r.stderr)


if __name__ == '__main__':
    unittest.main()
