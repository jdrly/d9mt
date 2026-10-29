#!/usr/bin/env bash
# test-air-versions.sh — the runtime metallibs' VERS tags for each macOS. Every corpus shader
# (air/tests/bin/*.bin) goes through the d9mtair compile path (airlib_loopback, the AirBuilder and
# metallib writer that winemetal.so links) three times: with D9MT_AIR_MACOS=26, with
# D9MT_AIR_MACOS=27 and without it. Apple's `metallib` then wraps the same bitcode.
#  - macOS 26: the header's OS version and every function's VERS tag equal Apple's (the bitcode's
#    own AIR version). Metal rejects a VERS newer than the OS supports (macOS 26: AIR 2.8).
#  - macOS 27: VERS AIR 2.9 with Apple's language version, the tag d9mt has always written there
#    (Metal's shader caches keep their keys); the file differs from the macOS 26 one only there.
#  - unset: byte-identical to the mode of the macOS this runs on.
# Needs build/airlib from scripts/build-bridge.sh (or build-d9mtair.sh) and Xcode's metal toolchain.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$V2/build/airlib"
WORK="${WORK:-$OUT/versions-test}"
LOOP="$OUT/airlib_loopback"
[ -x "$LOOP" ] && [ -f "$OUT/d9mtair.so" ] || { echo "FATAL: no $LOOP or d9mtair.so (run scripts/build-bridge.sh)" >&2; exit 1; }
xcrun --find metallib >/dev/null || { echo "FATAL: no metallib (Xcode's metal toolchain)" >&2; exit 1; }
HOST=$(sw_vers -productVersion | cut -d. -f1)
HOSTMODE=26; [ "$HOST" -ge 27 ] && HOSTMODE=27
rm -rf "$WORK"; mkdir -p "$WORK"
echo "macOS $HOST: the unset run must match the macOS $HOSTMODE mode"

pass=0; fail=0
for bin in "$V2"/air/tests/bin/*.bin; do
  name="$(basename "$bin" .bin)"
  w="$WORK/$name"
  if ! D9MT_AIR_MACOS=26 "$LOOP" "$OUT/d9mtair.so" "$bin" "$w.26.metallib" >/dev/null 2>&1 \
     || ! D9MT_AIR_MACOS=27 "$LOOP" "$OUT/d9mtair.so" "$bin" "$w.27.metallib" >/dev/null 2>&1 \
     || ! env -u D9MT_AIR_MACOS "$LOOP" "$OUT/d9mtair.so" "$bin" "$w.host.metallib" >/dev/null 2>&1; then
    printf '%-24s FAIL (compile)\n' "$name"; fail=$((fail+1)); continue
  fi
  python3 - "$w.26.metallib" "$w.air" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
off, size = struct.unpack_from('<QQ', d, 0x48)
open(sys.argv[2], 'wb').write(d[off:off+size])
EOF
  if ! xcrun metallib "$w.air" -o "$w.apple.metallib" 2> "$w.apple.err"; then
    printf '%-24s FAIL (metallib: %s)\n' "$name" "$(head -1 "$w.apple.err")"; fail=$((fail+1)); continue
  fi
  if result=$(python3 - "$w" "$HOSTMODE" <<'EOF'
import struct, sys

def tags(d):
    assert d[:4] == b'MTLB'
    os_version = struct.unpack_from('<HH', d, 12)
    header_size, list_size = struct.unpack_from('<QQ', d, 24)
    pos, end, vers = header_size + 4, header_size + list_size, []
    while pos < end:
        p, fn_end = pos + 4, pos + struct.unpack_from('<I', d, pos)[0]
        while p < fn_end and d[p:p+4] != b'ENDT':
            n = struct.unpack_from('<H', d, p + 4)[0]
            if d[p:p+4] == b'VERS':
                vers.append((p + 6, struct.unpack_from('<4H', d, p + 6)))
            p += 6 + n
        pos = fn_end
    return os_version, vers

w, hostmode = sys.argv[1], sys.argv[2]
m26, m27, host, apple = (open('%s.%s.metallib' % (w, k), 'rb').read() for k in ('26', '27', 'host', 'apple'))
t26, t27, ta = tags(m26), tags(m27), tags(apple)
fmt = lambda t: 'os %d.%d VERS %s' % (t[0] + (' '.join('%d.%d/%d.%d' % v for _, v in t[1]),))
errors = []
if not t26[1] or (t26[0], [v for _, v in t26[1]]) != (ta[0], [v for _, v in ta[1]]):
    errors.append('macOS 26 %s != Apple %s' % (fmt(t26), fmt(ta)))
if t27[0] != ta[0] or [v for _, v in t27[1]] != [(2, 9) + v[2:] for _, v in ta[1]]:
    errors.append('macOS 27 %s, not AIR 2.9 with Apple\'s language' % fmt(t27))
minors = {off + 2 for off, _ in t26[1]} | {off + 3 for off, _ in t26[1]}
if len(m26) != len(m27) or any(a != b and i not in minors for i, (a, b) in enumerate(zip(m26, m27))):
    errors.append('macOS 26 and 27 files differ outside the VERS AIR minor')
if host != (m27 if hostmode == '27' else m26):
    errors.append('unset differs from the macOS %s mode' % hostmode)
print('; '.join(errors) or '26: %s = Apple; 27: %s' % (fmt(t26), fmt(t27)))
sys.exit(1 if errors else 0)
EOF
  ); then
    printf '%-24s PASS (%s)\n' "$name" "$result"; pass=$((pass+1))
  else
    printf '%-24s FAIL (%s)\n' "$name" "$result"; fail=$((fail+1))
  fi
done
echo "----"
echo "pass=$pass fail=$fail"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
