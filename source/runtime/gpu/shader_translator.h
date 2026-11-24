#pragma once

// Host shader compilation for the D3D replacement layer (gpu/video.cpp).
//
// TranslateXenosShader: an Xbox 360 shader container (the blob passed to CreateVertexShader /
// CreatePixelShader, magic 0x102A11xx) -> HLSL via XenosRecomp's ShaderRecompiler (linked in
// from tools/XenosRecomp, built in-process) -> SPIR-V via DXC.
// CompileHlslToSpirv: plain HLSL -> SPIR-V (used for the runtime's own blit shaders).
//
// This file is compiled in two worlds: the runtime (which only sees this header) and the
// tl_shader_translator library (which also includes XenosRecomp's pch.h). Keep it free of
// runtime and XenosRecomp types.

#include <cstdint>
#include <string>
#include <vector>

namespace ShaderTranslator
{
    struct XenosResult
    {
        bool isPixelShader = false;
        uint32_t specConstantsMask = 0;
        std::string hlsl;
        std::vector<uint8_t> spirv;
        std::string error;
    };

    // shaderCommon: contents of XenosRecomp's shader_common.h (prepended to every shader).
    bool TranslateXenosShader(const uint8_t* container, const std::string& shaderCommon, XenosResult& result);

    // stage: 'v' vertex, 'p' pixel. invertY: add -fvk-invert-y (vertex shaders that output D3D clip space).
    bool CompileHlslToSpirv(const std::string& source, char stage, bool invertY, std::vector<uint8_t>& spirv, std::string& error);

    bool IsCompilerAvailable();
}
