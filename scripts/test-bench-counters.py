#!/usr/bin/env python3
"""scripts/bench-counters.py on synthetic logs: lines in the formats d9mt prints
(driver/backend/mtl_profiler.h, mtl_profiler_scope.h, driver/d3d9/d3d9_stats.h). No Wine, no GPU."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name('bench-counters.py')

PROF_OLD = ('info:  PROF frame={frame} win=256 ms(avg/f): beginRecording=1.527(0.00596484) prepareDraw=37.387(0.146043) '
            'flushChunk=4432.65(17.315) submit=1494.14(5.83648) endFrame=0.227(0.000886719) | draws={draws}({draws_f}/f) '
            'chunkFlush=371(1.44922/f) psoMiss=232 argRotate=372(1.45312/f) passClose/f: submit=0.222656 rt=1.22656 '
            'clear=1.54688 xfer=0 | framePasses=0/f storeDontCare=0/f vtxFence=0/f evRead=1917(7.48828/f) '
            'unsubWait=0(0/f) unsubWaitUs=12(max 7)')
CS = (' | csExec=812.345(3.173) csChunks=2048(8.00/f) csInject=0(0.00/f) csCmds={cmds}({cmds_f:.1f}/f) '
      'csKiB=9216.0(36.0/f) csSleep=3(0.01/f) | allocWait=1(0.00/f) allocWaitMs=0.512(0.002)')
STATS = ('warn:  STATS t={t} fps=58.3 ms=17.1/20.2/25.0 slow33=0 slow50=0 | rp=1.0 skip=0.0 cb=2.0 draws={draws} '
         '| ld=0.0 clr=1.0 ldDc=0.0 st=1.0 stDc=0.0 rsv=0.0 restore=0.0 savedMiB=0.0 attMs=0.12 clrFold=0.0 '
         'clrPass=0.0 | sh=0 pso=0 wait=2/3.5ms | allocMiB=96 att={att} | split=0.0 splitSubmit=0.0 splitXfer=0.0 '
         'splitClear=0.0 splitMiB=0.0 carried=0.0 carryRefused=0.0 carryCap=0.0 | flushChunk=1.0/0.0 '
         'flushRt=0.0/0.0 flushPresent=1.0/0.5 flushCapped=0.0 | at=12:00:01.250')


def prof(frame=512, draws=1536000, cmds=3072000, cs=True):
    line = PROF_OLD.format(frame=frame, draws=draws, draws_f=draws / 256)
    return line + (CS.format(cmds=cmds, cmds_f=cmds / 256) if cs else '')


def run(*logs):
    r = subprocess.run([sys.executable, '-I', str(SCRIPT), *logs], capture_output=True, text=True)
    rows = {}
    for line in r.stdout.splitlines()[1:]:
        cols = line.split('\t')
        rows[(cols[0], cols[1])] = cols[2:]
    return r.returncode, rows, r.stderr


class Counters(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.dir.cleanup()

    def log(self, name, *lines):
        p = Path(self.dir.name) / name
        p.write_text('\n'.join(['D9MT: profiler clock: test', *lines]) + '\n')
        return str(p)

    def test_one_more_command_per_draw(self):
        # the issue's control for part A: one more CS command per draw raises csCmds/f by the draws
        base = self.log('b', prof(frame=256, cmds=1), STATS.format(t=1, draws=6000, att='on'), prof(),
                        STATS.format(t=2, draws=6000, att='on'))
        head = self.log('h', prof(cmds=3072000 + 1536000), STATS.format(t=2, draws=6000, att='on'))
        rc, rows, err = run(base, head)
        self.assertEqual(rc, 0, err)
        self.assertEqual(rows[('PROF', 'csCmds/f')], ['12000', '18000', '+6000'])
        self.assertEqual(rows[('PROF', 'draws/f')], ['6000', '6000', '+0'])
        self.assertEqual(rows[('STATS', 'draws')], ['6000', '6000', '+0'])

    def test_counters_only(self):
        rc, rows, err = run(self.log('b', prof(), STATS.format(t=2, draws=6000, att='on')),
                            self.log('h', prof(), STATS.format(t=2, draws=6000, att='on')))
        self.assertEqual(rc, 0, err)
        names = {k[1] for k in rows}
        for timing in ('beginRecording', 'flushChunk', 'csExec', 'allocWaitMs', 'unsubWaitUs', 't', 'fps', 'ms',
                       'slow33', 'attMs', 'at'):
            self.assertFalse({n for n in names if n.split('/')[0] == timing}, timing)
        self.assertEqual(rows[('PROF', 'passClose.rt')][:2], ['1.22656', '1.22656'])
        self.assertEqual(rows[('PROF', 'psoMiss')][:2], ['232', '232'])
        self.assertEqual(rows[('PROF', 'framePasses/f')][:2], ['0', '0'])
        self.assertEqual(rows[('PROF', 'chunkFlush/f')][:2], ['1.449219', '1.449219'])
        self.assertEqual(rows[('STATS', 'wait')][:2], ['2', '2'])
        self.assertEqual(rows[('STATS', 'flushPresent.refused')][:2], ['0.5', '0.5'])
        self.assertEqual(rows[('STATS', 'att')], ['on', 'on', ''])

    def test_repeat_and_text(self):
        b = self.log('b', prof(), STATS.format(t=2, draws=6000, att='on'))
        h = self.log('h', prof(), STATS.format(t=2, draws=6000, att='off'))
        h2 = self.log('h2', prof(cmds=3072256), STATS.format(t=2, draws=6000, att='off'))
        rc, rows, err = run(b, h, '--repeat', b, h2)
        self.assertEqual(rc, 0, err)
        self.assertEqual(rows[('STATS', 'att')], ['on', 'off', 'differs', 'yes'])
        self.assertEqual(rows[('PROF', 'csCmds/f')], ['12000', '12000', '+0', 'no: base 12000, head 12001'])

    def test_older_base(self):
        rc, rows, err = run(self.log('b', prof(cs=False), STATS.format(t=2, draws=6000, att='on')),
                            self.log('h', prof(), STATS.format(t=2, draws=6000, att='on')))
        self.assertEqual(rc, 0, err)
        self.assertEqual(rows[('PROF', 'csCmds/f')], ['-', '12000', ''])

    def test_unprofiled_run_fails(self):
        rc, _, err = run(self.log('b', STATS.format(t=2, draws=6000, att='on')),
                         self.log('h', prof(), STATS.format(t=2, draws=6000, att='on')))
        self.assertEqual(rc, 1)
        self.assertIn('no PROF', err)
        rc, _, _ = run(self.log('b', prof()), self.log('h', prof()))
        self.assertEqual(rc, 1)

    def test_usage(self):
        self.assertEqual(run('a')[0], 2)


if __name__ == '__main__':
    unittest.main()
