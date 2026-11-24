# Building from source

You need your own copy of the game: the translated CPU code is generated from the game's `default.xex` and is not
part of this repository.

## Tools

- Windows 10 / 11 x64
- Visual Studio 2022 (C++ workload; the build uses its `clang-cl`)
- CMake 3.20+ and Ninja
- Vulkan SDK 1.3 or newer
- Git and Python 3.12

## Layout

The CMake files expect this layout (the names under `tools` matter):

```
<project folder>\
  src\                      <- the contents of this repository's source\ folder
  game\                     <- your unpacked game (default.xex, Data360*.forge, ...)
  tools\
    XenonRecomp\            <- https://github.com/hedge-dev/XenonRecomp (built, see below)
    XenosRecomp\            <- https://github.com/hedge-dev/XenosRecomp
    UnleashedRecomp\        <- https://github.com/hedge-dev/UnleashedRecomp (only its thirdparty\ folder is used)
    ffmpeg\                 <- an LGPL shared FFmpeg build for Windows x64 (bin\, include\, lib\)
  builds\
```

Clone the three hedge-dev repositories with `--recursive` (they use submodules). FFmpeg: for example the
`ffmpeg-n8.1-latest-win64-lgpl-shared` build from https://github.com/BtbN/FFmpeg-Builds.

## Steps

From a "x64 Native Tools Command Prompt for VS 2022", in `<project folder>`:

1. **Build the tools** (XenonRecomp and XenosRecomp), Release, with clang-cl and Ninja:
   ```
   cmake -S tools\XenonRecomp -B tools\XenonRecomp\build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
   cmake --build tools\XenonRecomp\build
   cmake -S tools\XenosRecomp -B tools\XenosRecomp\build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
   cmake --build tools\XenosRecomp\build
   ```
2. **Translate the game's code.** `src\config\FS.toml` points at `..\..\game\default.xex` and writes to
   `..\..\builds\ppc` (relative paths; run it from `src\config`):
   ```
   cd src\config
   ..\..\tools\XenonRecomp\build\XenonRecomp\XenonRecomp.exe FS.toml ..\..\tools\XenonRecomp\XenonUtils\ppc_context.h
   cd ..\..
   ```
   This writes about 370 `ppc_recomp.*.cpp` files (220 MB) into `builds\ppc`. They contain translated game code:
   keep them to yourself.
3. **Build the program** (the recompiled code library is built as part of it):
   ```
   cmake -S src -B builds\runtime -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DFS_BUILD_PPC=ON
   cmake --build builds\runtime --target ForgottenSandsRecomp
   ```
   The first build takes a long time (the translated code is large).
4. Copy `avcodec-62.dll`, `avformat-62.dll`, `avutil-60.dll`, `swresample-6.dll` from `tools\ffmpeg\bin` and
   `dxcompiler.dll` from `tools\XenosRecomp\thirdparty\dxc-bin\bin\x64` next to `builds\runtime\ForgottenSandsRecomp.exe`
   and start it with `--game <project folder>\game`.

## Useful options for development

`--game <folder>`, `--headless` (no window), `--timeout <seconds>`, `--shot <seconds> <file.bmp>`,
`--input-script <file>` (scripted controller input, see `source/runtime/hid/input_script.cpp`), `--any-xex` (skip the
game version check). Many diagnostic switches are environment variables named `FS_*`; they are described in the
comments where the code reads them.
