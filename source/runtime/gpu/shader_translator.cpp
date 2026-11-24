// Built inside the tl_shader_translator library with XenosRecomp's pch.h force-included
// (see src/CMakeLists.txt). Wraps XenosRecomp's ShaderRecompiler and DXC.
#include "shader_recompiler.h"
#include "shader_translator.h"

#include <mutex>

namespace ShaderTranslator
{
    // Step 35i: one compiler per thread (shaders are translated on several background threads at once; a
    // DXC compiler object must not be shared between threads, separate objects may run side by side).
    static thread_local IDxcCompiler3* t_dxcCompiler = nullptr;
    static thread_local bool t_dxcTried = false;

    static IDxcCompiler3* GetCompiler()
    {
        if (!t_dxcTried)
        {
            t_dxcTried = true;
            if (FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&t_dxcCompiler))))
                t_dxcCompiler = nullptr;
        }
        return t_dxcCompiler;
    }

    bool IsCompilerAvailable()
    {
        return GetCompiler() != nullptr;
    }

    bool CompileHlslToSpirv(const std::string& source, char stage, bool invertY, std::vector<uint8_t>& spirv, std::string& error)
    {

        IDxcCompiler3* compiler = GetCompiler();
        if (compiler == nullptr)
        {
            error = "DXC (dxcompiler) is not available";
            return false;
        }

        DxcBuffer buffer{};
        buffer.Ptr = source.c_str();
        buffer.Size = source.size();

        const wchar_t* args[16]{};
        uint32_t argCount = 0;
        args[argCount++] = stage == 'p' ? L"-T ps_6_0" : L"-T vs_6_0";
        args[argCount++] = L"-HV 2021";
        args[argCount++] = L"-all-resources-bound";
        args[argCount++] = L"-spirv";
        args[argCount++] = L"-fvk-use-dx-layout";
        if (invertY)
            args[argCount++] = L"-fvk-invert-y";
        args[argCount++] = L"-Qstrip_debug";

        IDxcResult* result = nullptr;
        HRESULT hr = compiler->Compile(&buffer, args, argCount, nullptr, IID_PPV_ARGS(&result));
        if (FAILED(hr) || result == nullptr)
        {
            error = "DXC Compile() call failed";
            return false;
        }

        HRESULT status = E_FAIL;
        result->GetStatus(&status);

        bool ok = false;
        if (FAILED(status))
        {
            IDxcBlobUtf8* errors = nullptr;
            if (result->HasOutput(DXC_OUT_ERRORS) && SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors != nullptr)
            {
                error.assign(errors->GetStringPointer(), errors->GetStringLength());
                errors->Release();
            }
            else
            {
                error = "DXC compilation failed (no error text)";
            }
        }
        else
        {
            IDxcBlob* object = nullptr;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) && object != nullptr)
            {
                auto data = reinterpret_cast<const uint8_t*>(object->GetBufferPointer());
                spirv.assign(data, data + object->GetBufferSize());
                object->Release();
                ok = !spirv.empty();
            }
            if (!ok)
                error = "DXC produced no object";
        }

        result->Release();
        return ok;
    }

    bool TranslateXenosShader(const uint8_t* container, const std::string& shaderCommon, XenosResult& out)
    {
        auto shaderContainer = reinterpret_cast<const ShaderContainer*>(container);
        if ((shaderContainer->flags & 0xFFFFFF00) != 0x102A1100 || shaderContainer->constantTableOffset == 0 || shaderContainer->shaderOffset == 0)
        {
            out.error = fmt::format("not a Xenos shader container (flags 0x{:08X})", uint32_t(shaderContainer->flags));
            return false;
        }

        ShaderRecompiler recompiler;
        recompiler.recompile(container, shaderCommon);

        out.isPixelShader = recompiler.isPixelShader;
        out.specConstantsMask = recompiler.specConstantsMask;
        out.hlsl = std::move(recompiler.out);

        return CompileHlslToSpirv(out.hlsl, out.isPixelShader ? 'p' : 'v', !out.isPixelShader, out.spirv, out.error);
    }
}
