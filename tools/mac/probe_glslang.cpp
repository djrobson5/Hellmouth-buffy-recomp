// probe_glslang.cpp: GLSL to SPIR-V with the glslang and SPIRV-Tools libraries of tools/mac/build_libs.sh, the way the renderer links them
// (static, the optimizer on). Built and run by tools/mac/check_mac_deps.sh. Exit 0 with SPIR-V made; 1 otherwise.
#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include <cstdio>
#include <vector>

int main() {
    glslang::InitializeProcess();
    const char* src = "#version 450\nlayout(location=0) in vec2 uv; layout(location=0) out vec4 c;\n"
                      "layout(binding=0) uniform sampler2D t;\nvoid main() { c = texture(t, uv) * 2.0 + vec4(0.25); }\n";
    glslang::TShader shader(EShLangFragment);
    shader.setStrings(&src, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangFragment, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    if (!shader.parse(GetDefaultResources(), 450, false, EShMsgDefault)) { std::fputs(shader.getInfoLog(), stderr); return 1; }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) { std::fputs(program.getInfoLog(), stderr); return 1; }
    std::vector<unsigned> spv;
    glslang::SpvOptions options;
    options.disableOptimizer = false;   // runs SPIRV-Tools' optimizer
    options.optimizeSize = true;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment), spv, &options);
    std::printf("spir-v: %zu words, magic %08x\n", spv.size(), spv.empty() ? 0u : spv[0]);
    glslang::FinalizeProcess();
    return spv.size() > 5 && spv[0] == 0x07230203 ? 0 : 1;
}
