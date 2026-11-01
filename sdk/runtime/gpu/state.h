#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// Everything the title handed to the XDK Direct3D API, as the API defines it. The renderer translates
// this to D3D12; nothing here models the console's GPU.
namespace gpu {

// D3DRS indices of the XDK 4432 build this title links, taken from its own tables (docs/xdk-4432.md). Simple states end
// at 83 (D3DDevice_SetRenderStateNotInline 0x591465), deferred at 118 (0x591489), complex at 147, named one by one by
// g_ComplexRenderStateFunctionTable. 4432 has no PresentationInterval state, and no SampleAlpha: its slot sits past the
// library's table, and the host's setter has no guest function to replace.
enum : uint32_t {
    kRsDeferredFirst = 83,
    kRsFogEnable = 83,
    kRsFogTableMode = 84,
    kRsFogStart = 85,
    kRsFogEnd = 86,
    kRsFogDensity = 87,
    kRsRangeFogEnable = 88,
    kRsWrap0 = 89,
    kRsLighting = 93,
    kRsSpecularEnable = 94,
    kRsLocalViewer = 95,
    kRsColorVertex = 96,
    kRsSpecularMaterialSource = 101,
    kRsDiffuseMaterialSource = 102,
    kRsAmbientMaterialSource = 103,
    kRsEmissiveMaterialSource = 104,
    kRsAmbient = 106,
    kRsPointSize = 107,
    kRsPointSizeMin = 108,
    kRsPointSpriteEnable = 109,
    kRsPointScaleEnable = 110,
    kRsPointScaleA = 111,
    kRsPointScaleB = 112,
    kRsPointScaleC = 113,
    kRsPointSizeMax = 114,
    kRsComplexFirst = 118,
    kRsPsTextureModes = 118,
    kRsVertexBlend = 119,
    kRsFogColor = 120,
    kRsFillMode = 121,
    kRsBackFillMode = 122,
    kRsTwoSidedLighting = 123,
    kRsNormalizeNormals = 124,
    kRsZEnable = 125,
    kRsStencilEnable = 126,
    kRsStencilFail = 127,
    kRsFrontFace = 128,
    kRsCullMode = 129,
    kRsTextureFactor = 130,
    kRsZBias = 131,
    kRsLogicOp = 132,
    kRsEdgeAntiAlias = 133,
    kRsMultiSampleAntiAlias = 134,
    kRsMultiSampleMask = 135,
    kRsMultiSampleMode = 136,
    kRsMultiSampleRenderTargetMode = 137,
    kRsShadowFunc = 138,
    kRsLineWidth = 139,
    kRsDxt1NoiseEnable = 140,
    kRsYuvEnable = 141,
    kRsOcclusionCullEnable = 142,
    kRsStencilCullEnable = 143,
    kRsRopZCmpAlwaysRead = 144,
    kRsRopZRead = 145,
    kRsDoNotCullUncompressed = 146,
    kRsSampleAlpha = 147,
    kRsMax = 148,
};

// Simple render states reach the API as (method, value) pairs; the method is the key the XDK's own
// D3DSIMPLERENDERSTATEENCODE table assigns to each state.
enum : uint32_t {
    kMAlphaTestEnable = 0x300,
    kMBlendEnable = 0x304,
    kMDitherEnable = 0x310,
    kMAlphaFunc = 0x33C,
    kMAlphaRef = 0x340,
    kMSrcBlend = 0x344,
    kMDestBlend = 0x348,
    kMBlendColor = 0x34C,
    kMBlendOp = 0x350,
    kMZFunc = 0x354,
    kMColorWriteEnable = 0x358,
    kMZWriteEnable = 0x35C,
    kMStencilWriteMask = 0x360,
    kMStencilFunc = 0x364,
    kMStencilRef = 0x368,
    kMStencilMask = 0x36C,
    kMStencilZFail = 0x374,
    kMStencilPass = 0x378,
    kMShadeMode = 0x37C,
    kMSolidOffsetEnable = 0x338,
    kMSlopeScale = 0x384,
    kMDepthOffset = 0x388,
    kMPsAlphaInputs = 0x260,     // 8
    kMPsFinalAbcd = 0x288,
    kMPsFinalEfgh = 0x28C,
    kMPsConstant0 = 0xA60,       // 8
    kMPsConstant1 = 0xA80,       // 8
    kMPsAlphaOutputs = 0xAA0,    // 8
    kMPsRgbInputs = 0xAC0,       // 8
    kMPsCompareMode = 0x17F8,
    kMPsFinalConstant0 = 0x1E20,
    kMPsFinalConstant1 = 0x1E24,
    kMPsRgbOutputs = 0x1E40,     // 8
    kMPsCombinerCount = 0x1E60,
    kMPsDotMapping = 0x1E74,
    kMPsInputTexture = 0x1E78,
    kMDepthClipControl = 0x1D78,
};

// texture stage state indices
enum : uint32_t {
    kTssAddressU = 0, kTssAddressV, kTssAddressW, kTssMagFilter, kTssMinFilter, kTssMipFilter, kTssMipLodBias,
    kTssMaxMipLevel, kTssMaxAnisotropy, kTssColorKeyOp, kTssColorSign, kTssAlphaKill, kTssColorOp, kTssColorArg0,
    kTssColorArg1, kTssColorArg2, kTssAlphaOp, kTssAlphaArg0, kTssAlphaArg1, kTssAlphaArg2, kTssResultArg,
    kTssTextureTransformFlags, kTssBumpEnvMat00, kTssBumpEnvMat01, kTssBumpEnvMat11, kTssBumpEnvMat10,
    kTssBumpEnvLScale, kTssBumpEnvLOffset, kTssTexCoordIndex, kTssBorderColor, kTssColorKeyColor, kTssCount = 32
};

struct VertexShader {
    uint32_t handle = 0;       // guest handle, record address | 1
    std::vector<uint32_t> decl;
    std::vector<uint32_t> program;  // 4 dwords per instruction
    uint32_t program_type = 0;      // 0x2078 vertex, 0x7378 state, 0x7778 read/write
    uint64_t hash = 0;
    // stream layout parsed from the declaration: register -> stream, offset, type
    struct Input {
        int stream = -1;
        uint32_t offset = 0;
        uint32_t type = 0;  // D3DVSDT
    };
    std::array<Input, 16> inputs{};
    std::array<uint32_t, 16> stream_stride{};  // bytes consumed per stream by the declaration
    std::vector<std::pair<uint32_t, std::array<float, 4>>> constants;  // D3DVSD_CONST tokens
};

struct PixelShader {
    uint32_t handle = 0;
    uint32_t def[0xF0 / 4] = {};  // D3DPIXELSHADERDEF
};

struct Viewport {
    uint32_t x = 0, y = 0, w = 640, h = 480;
    float min_z = 0, max_z = 1;
};

struct Surface {
    uint32_t header = 0;  // guest D3DSurface
    uint32_t data = 0, format = 0, size = 0;
};

struct State {
    std::array<uint32_t, 0x800> simple{};  // simple render states indexed by method / 4
    std::array<uint32_t, kRsMax> rs{};     // deferred and complex render states by D3DRS
    std::array<std::array<uint32_t, kTssCount>, 4> tss{};
    std::array<uint32_t, 4> textures{};    // guest D3DBaseTexture pointers
    std::array<uint32_t, 4> palettes{};    // guest D3DPalette pointers, the colours of the stage's P8 texture
    struct Stream {
        uint32_t vb = 0;  // guest D3DVertexBuffer
        uint32_t stride = 0;
    };
    std::array<Stream, 16> streams{};
    uint32_t index_buffer = 0, index_data = 0, base_vertex = 0;
    uint32_t vs_handle = 0;  // programmable when bit 0 set, otherwise an FVF
    std::shared_ptr<VertexShader> vs;
    std::shared_ptr<PixelShader> ps;
    std::array<std::array<float, 4>, 192> vsc{};  // hardware order: D3D register + 96
    std::array<std::array<float, 4>, 16> psc{};
    Viewport viewport;
    Surface rt, zs;
    uint32_t scissor_count = 0;
    std::array<uint32_t, 4 * 8> scissors{};
    // fixed function
    std::array<std::array<float, 16>, 512> transforms{};  // by D3DTRANSFORMSTATETYPE
    std::array<float, 17> material{};
    std::array<std::array<uint32_t, 26>, 8> lights{};  // D3DLIGHT8
    uint32_t light_enable = 0;                         // bit per light
    float ss_offset_x = 0, ss_offset_y = 0;
    float zscale = 16777215.0f;  // depth buffer units per 1.0
};

}  // namespace gpu
