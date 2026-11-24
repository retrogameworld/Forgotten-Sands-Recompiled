# Forgotten Sands Recompiled

An unofficial native Windows PC port of **Prince of Persia: The Forgotten Sands** (Xbox 360), made by static
recompilation. The game's Xbox 360 program was translated into native x86-64 code, and the console's graphics,
sound, file and system calls were rebuilt for Windows with Vulkan and SDL. It is not an emulator: the game runs as a
normal PC program.

**No game files are included.** You need your own Xbox 360 disc of the game, dumped to a disc image (.iso).

This project is not affiliated with or endorsed by Ubisoft. Prince of Persia is a trademark of Ubisoft
Entertainment.

## Features

- Runs natively on Windows 10 / 11 (64-bit), Vulkan graphics
- Real high-resolution rendering: the game's 720p picture is drawn at your screen's resolution (1080p, 1440p, 4K) or
  any size you choose, not upscaled
- 60 and 120 frames per second (or uncapped), with the game speed unchanged
- Smooth menus and HUD at high frame rates (the game's Flash menus are blended between their own 24-30 fps frames)
- FXAA anti-aliasing, borderless / exclusive fullscreen / windowed
- PlayStation button pictures and PC wording in the menus and tutorials
- The boot logos can be skipped
- Many fixes over the original at high frame rates (camera close-ups, shadow cascades, sun glare, effects)
- Shaders are translated in the background and kept in a cache, so later starts load faster

A controller is required (any XInput or PlayStation controller that SDL recognises). Keyboard and mouse are not
supported.

## Requirements

- Windows 10 or 11, 64-bit
- A CPU with AVX2 (Intel 4th gen / AMD Ryzen or newer)
- A GPU with Vulkan 1.2 and up-to-date drivers
- About 6 GB of free disk space next to the game (the disc image is unpacked once)
- Your own disc image of the **Xbox 360 disc release** of Prince of Persia: The Forgotten Sands (title ID
  5553083E). The program checks that the game's executable is exactly the version it was made from.

## Installation

1. Download `ForgottenSandsRecompiled-Windows-x64.zip` from the
   [Releases](../../releases) page and unpack it into a folder of its own (for example `C:\Games\Forgotten Sands`).
   Avoid `C:\Program Files` (the game writes its saves next to itself).
2. Put your disc image (`.iso`) of the game into that same folder.
3. Start `ForgottenSandsRecomp.exe`. At the first start the disc image is unpacked into a `game` folder (about 5 GB,
   usually under a minute). After that the `.iso` is no longer needed; you can move or delete it.

If you already have the game unpacked (the disc's files, with `default.xex` in them), you can copy those files into a
`game` folder next to the exe instead of using a disc image.

### Making a disc image

Dump the disc with an Xbox 360 that can read discs to a USB drive or network (for example with a dashboard file
manager), or with a compatible PC DVD drive and a dumping tool for Xbox 360 discs. The image should be a full disc
image (about 7.3 GB) or a trimmed XISO; both are supported. Downloaded copies are not supported and must not be used.

## Settings

All settings are in `settings.toml` next to the exe. Open it with Notepad; every option is explained inside. Changes
apply at the next start. The most important ones:

| Setting | What it does |
|---|---|
| `resolution` | `"auto"` (your screen's resolution, default), `"1080p"`, `"1440p"`, `"4k"` or any `"WIDTHxHEIGHT"` |
| `fps_limit` | `120` (default), `60`, or `0` for no limit |
| `vsync` | `true` = no tearing, the screen's refresh rate is the upper limit |
| `display_mode` | `"borderless"` (default), `"fullscreen"` or `"windowed"` (Alt+Enter switches) |
| `antialiasing` | `"fxaa_extreme"` (default), `"fxaa"` or `"off"` |
| `vibration` | controller rumble on / off |

Saves and the shader cache are kept in the `user` folder next to the exe, the log in `ForgottenSandsRecomp.log` next to it.
Delete `settings.toml` to get the default settings back.

## How it works

- **CPU:** the game's PowerPC program (`default.xex`) was translated function by function into C++ with
  [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and compiled for x86-64. The program you download contains
  that translated code; it only runs together with the game executable from your own disc, which is loaded at start
  for its data and checked against the version the code was made from.
- **Graphics:** the game's Direct3D 9-style calls are rebuilt on Vulkan (based on the runtime of
  [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp)), its Xenos shaders are translated to SPIR-V with
  [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) and DirectXShaderCompiler, and render targets are scaled per
  axis for real high-resolution output.
- **Sound and video:** XMA audio and Bink movies are decoded with FFmpeg.
- **System:** the Xbox 360 kernel and XAM calls the game uses (files, threads, memory, saves, controller) are
  implemented natively.

The full source code is in [`source/`](source); [BUILDING.md](BUILDING.md) explains how to build it from your own
game files.

## Known limitations

- Online features of the original are not available.
- Movies play at their original frame rate.
- Only the Xbox 360 disc version above is supported (no title updates, no other platforms).

## Licence

The source code in this repository is released under the GNU General Public License v3.0 (see [LICENSE](LICENSE)),
because the runtime is derived from UnleashedRecomp's (GPL-3.0). Third-party components and their licences are listed
in [THIRD_PARTY.md](THIRD_PARTY.md). No part of the game itself is included.
