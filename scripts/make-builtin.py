#!/usr/bin/env python3
"""Mark a PE dll as a wine builtin: write the 16-byte signature
"Wine builtin DLL" at offset 0x40 (inside the DOS stub), exactly what
`winebuild --builtin` produces. Requires e_lfanew >= 0x50.

From neo773's d9mt (https://github.com/neo773/d9mt, tools/make-builtin.py)."""
import struct
import sys

# wine's server/mapping.c memcmps sizeof(signature) = 17 bytes including
# the NUL; winebuild zero-fills the whole 32-byte stub buffer after it
SIG = b"Wine builtin DLL".ljust(32, b"\0")

for path in sys.argv[1:]:
    with open(path, "r+b") as f:
        data = f.read(0x44)
        assert data[:2] == b"MZ", f"{path}: not a PE file"
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        assert e_lfanew >= 0x40 + len(SIG), f"{path}: DOS stub too small (e_lfanew={e_lfanew:#x})"
        f.seek(0x40)
        f.write(SIG)
    print(f"{path}: marked as wine builtin")
