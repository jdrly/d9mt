# Building

Details behind the build steps in the [README](../README.md#build). Run everything from the
repository root.

## Toolchain and targets

The scripts need bash ≥ 4.3 (`/bin/bash` 3.2 fails), so put Homebrew's first
in `PATH`.

LLVM and `winemetal.so` target macOS 26.0, whatever the SDK; set
`MACOSX_DEPLOYMENT_TARGET` to change it. An LLVM built before this default
(with the SDK's version) gives linker warnings: rebuild it. The embedded
metallibs come in two builds: the SDK's newest Metal language (MSL 4.1 with
Xcode 27) and a Compat build for that minimum. The driver loads the newest
and falls back to Compat where Metal rejects it (MSL 4.1 on macOS 26).

## The build scripts

- No DXVK or DXMT checkout and no Wine engine are needed to build;
  `driver/` and `bridge/` contain everything.
- `build-driver.sh` regenerates `driver/d3d9/d3d9_ff_metallib.h` and
  `driver/backend/mtl_blit_metallib.h` when their sources (or the script)
  are newer and `xcrun metal`, `glslangValidator` and `spirv-cross` are
  available; otherwise it keeps the committed headers. On Xcode 27 run
  `xcodebuild -downloadComponent MetalToolchain` once. If `xcode-select`
  points at the Command Line Tools, the script falls back to
  `/Applications/Xcode.app`.
- `D9MT_JOBS=<n>` (1–999) is the most compiles `build-driver.sh`,
  `build-air-compiler.sh` and `build-d9mtair.sh` run at once (default 12, 10
  and 10); `build-bridge.sh` passes it on to the `build-d9mtair.sh` it runs
  (its own compiles run one at a time). `D9MT_JOBS=1`
  compiles one file at a time and needs the least memory. The compiles and
  the link order are the same for any value (`scripts/test-build-jobs.sh`).
- `build-bridge.sh` ends with a smoke test that needs reference metallibs
  from `scripts/build-air-compiler.sh` (Homebrew `llvm@15` and a SPIRV-Cross
  checkout in `third_party/SPIRV-Cross`). Without them the test is skipped;
  `SMOKE=0` skips it explicitly.
- The driver and the bridge share an ABI: always rebuild and install them
  together.
- Two from-scratch builds of the same commit with the same toolchain give
  byte-identical `d3d9.dll`, `winemetal.dll`, `d9mtair.dll`, `.a` and `.o`
  files, wherever the checkout is: the PE links write no time stamp and use
  fixed image bases, the archives carry no dates, and the import libraries
  don't name the build directory. Check it with `shasum -a 256` over those
  files of both builds (`build/driver-x64`, `build/driver`, `build/bridge`,
  `build/airlib`).

## Release binaries

The checkout's path is mapped out of the binaries (`-ffile-prefix-map`; the
debug info still names the toolchain's own build paths), but the driver is
built with `-g` and is about 80 MB. Strip before installing or shipping:

```bash
x86_64-w64-mingw32-strip build/driver-x64/d3d9.dll
i686-w64-mingw32-strip   build/driver/d3d9.dll
strip -x build/bridge/winemetal.so
```

A binary release must carry the notices listed in
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
