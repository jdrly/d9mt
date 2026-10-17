"""Run test/code_sm3_ref.exe under Wine in a throwaway prefix (shared by
scripts/sm3_hazard_test.py and scripts/sm3_ref_test.py).

Environment:
  ENGINE      Wine engine root (bin/wine, bin/wineserver) whose winemetal is
              the bridge of the driver under test (scripts/install-bridge-
              builtins.sh into a dedicated engine copy)
  FRAMEWORKS  extra DYLD_FALLBACK_LIBRARY_PATH entry the engine needs (optional)
  WORK        scratch dir: the Wine prefix, the d9mt AIR cache and pipeline
              archives live there, nothing outside it is written (default:
              /tmp/d9mt-sm3-work)
"""
import os
import shutil
import subprocess

V2 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class WineRunner:
    def __init__(self, dll, arch='x64'):
        self.engine = os.environ.get('ENGINE')
        if not self.engine:
            raise SystemExit('set ENGINE to a Wine engine root (bin/wine inside)')
        self.work = os.environ.get('WORK', '/tmp/d9mt-sm3-work')
        self.prefix = os.path.join(self.work, 'prefix')
        self.wine = os.path.join(self.engine, 'bin', 'wine')
        self.wineserver = os.path.join(self.engine, 'bin', 'wineserver')
        self.env = dict(os.environ)
        fw = os.environ.get('FRAMEWORKS')
        self.env.update({
            'WINEPREFIX': self.prefix, 'WINEDEBUG': '-all', 'WINEMSYNC': '1',
            'DYLD_FALLBACK_LIBRARY_PATH': (fw + ':' if fw else '') + '/usr/lib',
            'WINEDLLOVERRIDES': 'd3d9=n,b',
            'D9MT_AIR_CACHE': os.path.join(self.work, 'air-cache.sqlite3'),
            'D9MT_PSO_ARCHIVE_DIR': os.path.join(self.work, 'pso'),
            'DXVK_LOG_LEVEL': 'info',
            'DXVK_LOG_PATH': 'C:\\d9mt-test\\log',
        })
        os.makedirs(self.work, exist_ok=True)
        if not os.path.exists(os.path.join(self.prefix, 'system.reg')):
            subprocess.run([self.wine, 'wineboot', '-i'], env=self.env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run([self.wineserver, '-w'], env=self.env)
        self.dir = os.path.join(self.prefix, 'drive_c', 'd9mt-test')
        os.makedirs(self.dir, exist_ok=True)
        cc = 'x86_64-w64-mingw32-gcc' if arch == 'x64' else 'i686-w64-mingw32-gcc'
        subprocess.run([cc, '-O1', os.path.join(V2, 'test', 'code_sm3_ref.c'), '-o',
                        os.path.join(self.dir, 'code_sm3_ref.exe'), '-ld3d9', '-lgdi32', '-luser32'],
                       check=True)
        shutil.copy(dll, os.path.join(self.dir, 'd3d9.dll'))

    def reset_dir(self, name):
        d = os.path.join(self.dir, name)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        return d

    def log_text(self):
        p = os.path.join(self.dir, 'log', 'code_sm3_ref_d3d9.log')
        return open(p, errors='replace').read() if os.path.exists(p) else ''

    def run(self, manifest_lines, extra_env=None, timeout=1800, conf=None):
        """Runs the manifest, restarting after a crash at the next item.
        Returns ({item index: output line}, {item index: 'crash'}, log text);
        self.last_ms is the harness's own time for the items (crash-free runs)."""
        shutil.rmtree(os.path.join(self.dir, 'log'), ignore_errors=True)
        os.makedirs(os.path.join(self.dir, 'log'))
        conf_path = os.path.join(self.dir, 'dxvk.conf')
        if conf:
            open(conf_path, 'w').write(conf)
        elif os.path.exists(conf_path):
            os.remove(conf_path)
        open(os.path.join(self.dir, 'manifest.txt'), 'w').write('\n'.join(manifest_lines) + '\n')
        count = sum(1 for l in manifest_lines if l.split()[:1] and l.split()[0] in ('create', 'case'))
        env = dict(self.env)
        env.update(extra_env or {})
        results, crashes, logs = {}, {}, []
        self.last_ms = 0.0
        first = 0
        while first < count:
            try:
                p = subprocess.run([self.wine, 'code_sm3_ref.exe', 'manifest.txt', str(first)],
                                   cwd=self.dir, env=env, capture_output=True, text=True,
                                   errors='replace', timeout=timeout)
                out = p.stdout
            except subprocess.TimeoutExpired as e:
                out = e.stdout.decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
            subprocess.run([self.wineserver, '-k'], env=env, stderr=subprocess.DEVNULL)
            subprocess.run([self.wineserver, '-w'], env=env)
            logs.append(self.log_text())
            last = first - 1
            done = False
            for line in out.splitlines():
                parts = line.split()
                if parts[:1] in (['RESULT'], ['CREATED']) and len(parts) > 1:
                    idx = int(parts[1])
                    results[idx] = line
                    last = max(last, idx)
                elif parts[:1] == ['DONE']:
                    done = True
                    if len(parts) > 2:
                        self.last_ms += float(parts[2])
            if done:
                break
            crashes[last + 1] = 'crash'
            first = last + 2
        return results, crashes, '\n'.join(logs)
