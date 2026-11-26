# Third-party components

| Component | Used for | Licence |
|---|---|---|
| [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) | base of the runtime (graphics layer, kernel, threads, memory) | GPL-3.0 |
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) | translation of the PowerPC code, `ppc_context.h` | MIT |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) | translation of the Xenos shaders | MIT |
| [plume](https://github.com/renderbag/plume) | Vulkan render interface (with occlusion query additions) | MIT |
| [SDL2](https://www.libsdl.org/) | window, controllers, audio output | zlib |
| [simde](https://github.com/simd-everywhere/simde) | portable SIMD for the translated code | MIT |
| [xxHash](https://github.com/Cyan4973/xxHash) | hashing | BSD-2-Clause |
| [fmt](https://github.com/fmtlib/fmt) | text formatting | MIT |
| [toml++](https://github.com/marzer/tomlplusplus) | settings file | MIT |
| [unordered_dense](https://github.com/martinus/unordered_dense), [concurrentqueue](https://github.com/cameron314/concurrentqueue) | containers | MIT / BSD |
| [Volk](https://github.com/zeux/volk), [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | Vulkan loading and memory | MIT |
| [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) (`dxcompiler.dll`) | HLSL to SPIR-V at run time | University of Illinois/NCSA Open Source License, Apache-2.0 with LLVM exception |
| [FFmpeg](https://ffmpeg.org/) (`avcodec`, `avformat`, `avutil`, `swresample` DLLs) | XMA audio and Bink video decoding | LGPL-3.0 |
| Microsoft Visual C++ runtime (`msvcp140*.dll`, `vcruntime140*.dll`) | C++ runtime (Windows) | Microsoft Visual C++ redistributable licence |
| LLVM libc++ (`libc++_shared.so`, from the Android NDK) | C++ runtime (Android) | Apache-2.0 with LLVM exception |

## FFmpeg

The release zip contains unmodified shared FFmpeg libraries from the LGPL build `n8.1.3-36-ge22a4510c9-20261118` of
[BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds) (FFmpeg git commit `e22a4510c9`). The FFmpeg source code
for that version is available at https://git.ffmpeg.org/ffmpeg.git and the build scripts at the BtbN repository. You
may replace the DLLs with your own compatible build. The LGPL-3.0 text is in `licenses/` in the release zip.

The Android app contains a small static FFmpeg 8.1.3 (LGPL-2.1 or later; git commit `1041abdc962f4cc4f394aa8de9dc5236c0c3b9e7`
of https://git.ffmpeg.org/ffmpeg.git) with only the Bink and XMA decoders and the Bink demuxer, built by
`source/android/build_ffmpeg.sh`, and DirectXShaderCompiler v1.8.2505.1 built from source by
`source/android/build_dxc.bat`. Because the whole app's source code (including these build scripts) is published here
under GPL-3.0, you can rebuild and relink it with your own FFmpeg build. The licence texts are inside the APK under
`assets/licenses/`.

## Changes to upstream projects

This project's changes to XenonRecomp, XenosRecomp, plume and SDL are in `source/patches/` as git patches, with the
upstream commit each one applies to in `source/patches/versions.txt`.
