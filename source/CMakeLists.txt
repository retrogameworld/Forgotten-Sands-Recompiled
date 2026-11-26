# Prince of Persia: The Forgotten Sands Recompiled - native runtime (ForgottenSandsRecomp.exe)
#
# Links the recompiled PowerPC code (ForgottenSandsPPC, from src/ppc_lib + builds/ppc) with the
# runtime in src/runtime (derived from UnleashedRecomp's generic runtime). Third-party code is
# referenced in place from tools/UnleashedRecomp and tools/XenonRecomp, never copied into src.
#
# Build: src/build.bat  (Ninja + clang-cl, output in builds/runtime)
cmake_minimum_required(VERSION 3.20)
project(ForgottenSandsRecomp C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Step 35 (60 fps): RelWithDebInfo's /Ob1 only inlined functions marked inline - xxhash's reads became
# memcpy calls and the renderer's small helpers real calls. Full inlining, debug info kept.
string(REPLACE "/Ob1" "/Ob2" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
string(REPLACE "/Ob1" "/Ob2" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
set(BUILD_SHARED_LIBS OFF)

set(FS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Forgotten Sands recomp root")
get_filename_component(FS_ROOT "${FS_ROOT}" ABSOLUTE)
if (ANDROID)
    set(FS_PPC_DIR "${FS_ROOT}/builds/ppc_arm") # Step 37: see src/ppc_lib/CMakeLists.txt
else()
    set(FS_PPC_DIR "${FS_ROOT}/builds/ppc")
endif()
set(UR_ROOT "${FS_ROOT}/tools/UnleashedRecomp")
set(UR_THIRDPARTY "${UR_ROOT}/thirdparty")
set(XR_ROOT "${FS_ROOT}/tools/XenonRecomp")
set(XR_THIRDPARTY "${XR_ROOT}/thirdparty")

# Step 36 (public release): no folder names of the build PC inside the program (source file names in asserts,
# the .pdb path).
if (MSVC)
    add_compile_options("/clang:-ffile-prefix-map=${FS_ROOT}=.")
    add_link_options("/PDBALTPATH:%_PDB%")
else()
    add_compile_options("-ffile-prefix-map=${FS_ROOT}=.") # Step 37: the same for the Android build
endif()

if (CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|amd64")
    add_compile_options(-march=x86-64-v3) # Step 35 (was sandybridge)
endif()
if (ANDROID)
    # Step 37: ARMv8.2 (LSE atomics - every phone CPU since 2018), full optimisation, no stack cookies
    add_compile_options(-march=armv8.2-a -O3 -fno-stack-protector)
endif()

# ------------------------------------------------------------------------------------------
# Recompiled code. Default: reuse the library already built by src/ppc_lib/build.bat
# (builds/ppc_lib/ForgottenSandsPPC.lib). FS_BUILD_PPC=ON builds it here from src/ppc_lib instead.
option(FS_BUILD_PPC "Build ForgottenSandsPPC as part of this project instead of using builds/ppc_lib" OFF)
if (FS_BUILD_PPC)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/ppc_lib" "${CMAKE_BINARY_DIR}/ppc_lib")
else()
    if (ANDROID)
        set(FS_PPC_LIB "${FS_ROOT}/builds/android_ppc/libForgottenSandsPPC.a") # Step 37: android\build_ppc.bat
    else()
        set(FS_PPC_LIB "${FS_ROOT}/builds/ppc_lib/ForgottenSandsPPC.lib")
    endif()
    if (NOT EXISTS "${FS_PPC_LIB}")
        message(FATAL_ERROR "${FS_PPC_LIB} not found. Run src/ppc_lib/build.bat first or configure with -DFS_BUILD_PPC=ON.")
    endif()
    add_library(ForgottenSandsPPC STATIC IMPORTED)
    set_target_properties(ForgottenSandsPPC PROPERTIES IMPORTED_LOCATION "${FS_PPC_LIB}")
    target_include_directories(ForgottenSandsPPC INTERFACE "${FS_PPC_DIR}" "${XR_THIRDPARTY}/simde")
endif()

# ------------------------------------------------------------------------------------------
# SDL2 (static, from UnleashedRecomp's submodule)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
add_subdirectory("${UR_THIRDPARTY}/SDL" "${CMAKE_BINARY_DIR}/SDL" EXCLUDE_FROM_ALL)

# plume: Vulkan backend only (portable; D3D12 is not used)
add_library(plume_vulkan STATIC "${UR_THIRDPARTY}/plume/plume_vulkan.cpp")
target_include_directories(plume_vulkan PUBLIC
    "${UR_THIRDPARTY}/plume"
    "${UR_THIRDPARTY}/plume/contrib/volk"
    "${UR_THIRDPARTY}/plume/contrib/Vulkan-Headers/include"
    "${UR_THIRDPARTY}/plume/contrib/VulkanMemoryAllocator/include")
target_compile_options(plume_vulkan PRIVATE -Wno-everything)
if (WIN32)
    target_compile_definitions(plume_vulkan PUBLIC NOMINMAX)
endif()
if (NOT ANDROID AND (CMAKE_SYSTEM_NAME MATCHES "Linux")) # Android: plume takes the ANativeWindow from SDL (video.cpp)
    target_compile_definitions(plume_vulkan PUBLIC SDL_VULKAN_ENABLED)
    target_link_libraries(plume_vulkan PUBLIC SDL2::SDL2-static)
endif()

# XEX loader (decryption + LZX) from XenonUtils
add_library(xex_loader STATIC
    "${XR_ROOT}/XenonUtils/xex.cpp"
    "${XR_ROOT}/XenonUtils/image.cpp"
    "${XR_ROOT}/XenonUtils/xex_patcher.cpp"
    "${XR_ROOT}/XenonUtils/memory_mapped_file.cpp"
    "${XR_THIRDPARTY}/libmspack/libmspack/mspack/lzxd.c"
    "${XR_THIRDPARTY}/tiny-AES-c/aes.c")
target_include_directories(xex_loader PUBLIC "${XR_ROOT}/XenonUtils" "${XR_THIRDPARTY}/simde"
    PRIVATE "${XR_THIRDPARTY}/libmspack/libmspack/mspack" "${XR_THIRDPARTY}/tiny-AES-c" "${XR_THIRDPARTY}/TinySHA1")
target_compile_definitions(xex_loader PRIVATE NOMINMAX)
target_compile_options(xex_loader PRIVATE -Wno-everything)

# small C libraries
add_library(fs_thirdparty STATIC
    "${UR_THIRDPARTY}/o1heap/o1heap.c"
    "${XR_THIRDPARTY}/xxHash/xxhash.c")
target_include_directories(fs_thirdparty PUBLIC "${UR_THIRDPARTY}/o1heap" "${XR_THIRDPARTY}/xxHash")
target_compile_options(fs_thirdparty PRIVATE -Wno-everything)

# ------------------------------------------------------------------------------------------
# Shader translator: XenosRecomp's ShaderRecompiler (built in-process) + DXC, used at runtime to
# turn Xenos shader microcode (CreateVertexShader/CreatePixelShader) into SPIR-V.
set(XS_ROOT "${FS_ROOT}/tools/XenosRecomp")
set(XS_THIRDPARTY "${XS_ROOT}/thirdparty")
set(XS_DXC "${XS_THIRDPARTY}/dxc-bin")
add_library(fs_shader_translator STATIC
    "${XS_ROOT}/XenosRecomp/shader_recompiler.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gpu/shader_translator.cpp")
if (ANDROID)
    # Step 37: the headers of the DXC that is built from source (the prebuilt dxc-bin's WinAdapter.h does not compile here)
    target_include_directories(fs_shader_translator BEFORE PRIVATE "${FS_ROOT}/tools/dxc-src/include/dxc" "${FS_ROOT}/tools/dxc-src/include")
endif()
target_include_directories(fs_shader_translator PRIVATE
    "${XS_ROOT}/XenosRecomp"
    "${XS_DXC}/inc"
    "${XS_THIRDPARTY}/smol-v/source"
    "${XS_THIRDPARTY}/zstd/lib"
    "${XR_THIRDPARTY}/fmt/include"
    "${XR_THIRDPARTY}/xxHash")
target_precompile_headers(fs_shader_translator PRIVATE "${XS_ROOT}/XenosRecomp/pch.h")
target_compile_definitions(fs_shader_translator PRIVATE FMT_HEADER_ONLY NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(fs_shader_translator PRIVATE -Wno-everything)
if (WIN32)
    target_compile_options(fs_shader_translator PRIVATE /EHsc)
    target_link_libraries(fs_shader_translator PUBLIC "${XS_DXC}/lib/x64/dxcompiler.lib")
    set(FS_DXC_DLL "${XS_DXC}/bin/x64/dxcompiler.dll")
elseif (ANDROID)
    # Step 37: DXC built for Android ARM64 from source (android\build_dxc.bat)
    set(FS_DXC_ANDROID "${FS_ROOT}/builds/android_dxc/lib/libdxcompiler.so")
    if (EXISTS "${FS_DXC_ANDROID}")
        target_link_libraries(fs_shader_translator PUBLIC "${FS_DXC_ANDROID}")
    else()
        message(WARNING "${FS_DXC_ANDROID} not built yet: shaders cannot be translated")
        target_compile_definitions(fs_shader_translator PRIVATE FS_NO_DXC)
    endif()
else()
    target_link_libraries(fs_shader_translator PUBLIC "${XS_DXC}/lib/x64/libdxcompiler.so")
endif()

# XenosRecomp's shader_common.h is prepended to every translated shader: embed it as a string.
file(READ "${XS_ROOT}/XenosRecomp/shader_common.h" FS_SHADER_COMMON_TEXT)
file(WRITE "${CMAKE_BINARY_DIR}/generated/shader_common_text.h"
    "// Generated by src/CMakeLists.txt from tools/XenosRecomp/XenosRecomp/shader_common.h
#pragma once
static const char g_shaderCommonText[] = R\"XENOSCOMMON(${FS_SHADER_COMMON_TEXT})XENOSCOMMON\";
")

# ------------------------------------------------------------------------------------------
# Runtime
set(RT "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
set(FS_RUNTIME_SOURCES
    "${RT}/main.cpp"
    "${RT}/stdafx.cpp"
    "${RT}/sdl_listener.cpp"
    "${RT}/kernel/memory.cpp"
    "${RT}/kernel/heap.cpp"
    "${RT}/kernel/xdm.cpp"
    "${RT}/kernel/imports.cpp"
    "${RT}/kernel/frame_time_hooks.cpp"
    "${RT}/kernel/camera_hooks.cpp"
    "${RT}/kernel/import_log.cpp"
    "${RT}/kernel/import_fallback.cpp"
    "${RT}/kernel/guest_printf.cpp"
    "${RT}/kernel/nt_memory.cpp"
    "${RT}/kernel/xecrypt.cpp"
    "${RT}/kernel/xex_loader.cpp"
    "${RT}/kernel/video_stubs.cpp"
    "${RT}/kernel/xam.cpp"
    "${RT}/kernel/io/file_system.cpp"
    "${RT}/kernel/io/iso_extract.cpp"
    "${RT}/kernel/text_tokens.cpp"
    "${RT}/cpu/guest_thread.cpp"
    "${RT}/cpu/host_setjmp.cpp"
    "${RT}/os/perf.cpp"
    "${RT}/apu/audio.cpp"
    "${RT}/apu/xma.cpp"
    "${RT}/apu/xaudio_hooks.cpp"
    "${RT}/apu/movie_hd.cpp"
    "${RT}/apu/bink_ffmpeg.cpp"
    "${RT}/apu/driver/sdl2_driver.cpp"
    "${RT}/hid/hid.cpp"
    "${RT}/hid/driver/sdl_hid.cpp"
    "${RT}/hid/input_script.cpp"
    "${RT}/gpu/video.cpp"
    "${RT}/os/logger.cpp"
    "${RT}/os/crash.cpp"
    "${RT}/user/config.cpp"
    "${RT}/user/paths.cpp")

if (WIN32)
    list(APPEND FS_RUNTIME_SOURCES "${RT}/os/win32/process_win32.cpp")
else()
    list(APPEND FS_RUNTIME_SOURCES "${RT}/os/linux/process_linux.cpp")
endif()

if (ANDROID)
    # Step 37: the Android app loads libmain.so and calls SDL_main (SDL's Java side, SDLActivity).
    add_library(ForgottenSandsRecomp SHARED ${FS_RUNTIME_SOURCES})
    set_target_properties(ForgottenSandsRecomp PROPERTIES OUTPUT_NAME main)
else()
    add_executable(ForgottenSandsRecomp ${FS_RUNTIME_SOURCES})
endif()
target_include_directories(ForgottenSandsRecomp PRIVATE
    "${RT}"
    "${XR_THIRDPARTY}/fmt/include"
    "${XR_THIRDPARTY}/tomlplusplus/include"
    "${UR_THIRDPARTY}/unordered_dense/include"
    "${UR_THIRDPARTY}/concurrentqueue"
    "${UR_THIRDPARTY}/ddspp"
    "${CMAKE_BINARY_DIR}/generated")
target_precompile_headers(ForgottenSandsRecomp PRIVATE "${RT}/stdafx.h")
target_compile_definitions(ForgottenSandsRecomp PRIVATE
    SDL_MAIN_HANDLED
    FMT_HEADER_ONLY
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
target_compile_options(ForgottenSandsRecomp PRIVATE
    -fno-strict-aliasing
    -Wno-switch
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-void-pointer-to-int-cast
    -Wno-int-to-void-pointer-cast
    -Wno-invalid-offsetof
    -Wno-null-arithmetic
    -Wno-null-conversion
    -Wno-tautological-undefined-compare
    -Wno-multichar
    -Wno-microsoft-cast
    -Wno-deprecated-declarations
    -Wno-nonportable-include-path
    -Wno-unused-private-field
    -Wno-unknown-pragmas
    -Wno-ignored-attributes)
if (WIN32)
    target_compile_options(ForgottenSandsRecomp PRIVATE /fp:strict /EHsc)
    # /FORCE:MULTIPLE: XenonRecomp emits 382 identical empty functions (0x8302548C-0x83027444,
    # data words at the end of .text) in two output files; the copies are identical.
    target_link_options(ForgottenSandsRecomp PRIVATE /MAP /STACK:8388608 /FORCE:MULTIPLE)
    target_link_libraries(ForgottenSandsRecomp PRIVATE winmm Synchronization)
else()
    target_compile_options(ForgottenSandsRecomp PRIVATE -ffp-model=strict)
    # the same identical duplicate functions as /FORCE:MULTIPLE above
    target_link_options(ForgottenSandsRecomp PRIVATE -Wl,--allow-multiple-definition)
endif()
if (ANDROID)
    target_link_libraries(ForgottenSandsRecomp PRIVATE android log)
    target_link_options(ForgottenSandsRecomp PRIVATE -Wl,-Bsymbolic) # FFmpeg's ARM64 assembly refers to its tables directly
endif()

target_link_libraries(ForgottenSandsRecomp PRIVATE
    ForgottenSandsPPC
    SDL2::SDL2-static
    plume_vulkan
    xex_loader
    fs_thirdparty
    fs_shader_translator)

if (FS_DXC_DLL)
    add_custom_command(TARGET ForgottenSandsRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FS_DXC_DLL}" "$<TARGET_FILE_DIR:ForgottenSandsRecomp>")
endif()

set_target_properties(ForgottenSandsRecomp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# FFmpeg (LGPL shared build, tools/ffmpeg) for XMA audio decoding (apu/xma.cpp).
if (ANDROID)
    # Step 37: a small static FFmpeg for Android ARM64 with only the decoders the game needs (android\build_ffmpeg.sh)
    set(FS_FFMPEG "${FS_ROOT}/builds/android_ffmpeg")
    target_include_directories(ForgottenSandsRecomp PRIVATE "${FS_FFMPEG}/include")
    target_link_libraries(ForgottenSandsRecomp PRIVATE "${FS_FFMPEG}/lib/libavformat.a" "${FS_FFMPEG}/lib/libavcodec.a" "${FS_FFMPEG}/lib/libswresample.a" "${FS_FFMPEG}/lib/libavutil.a" m z)
    set(FS_FFMPEG_DLLS "")
else()
set(FS_FFMPEG "${FS_ROOT}/tools/ffmpeg")
target_include_directories(ForgottenSandsRecomp PRIVATE "${FS_FFMPEG}/include")
target_link_libraries(ForgottenSandsRecomp PRIVATE "${FS_FFMPEG}/lib/avcodec.lib" "${FS_FFMPEG}/lib/avutil.lib" "${FS_FFMPEG}/lib/avformat.lib")
file(GLOB FS_FFMPEG_DLLS "${FS_FFMPEG}/bin/*.dll")
endif()
foreach(dll ${FS_FFMPEG_DLLS})
    add_custom_command(TARGET ForgottenSandsRecomp POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:ForgottenSandsRecomp>")
endforeach()
