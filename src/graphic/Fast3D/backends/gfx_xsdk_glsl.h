#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct CCFeatures;

namespace Fast {
// One vertex attribute after the position, as the interpreter writes it (interpreter.cpp:1617-1762): its first float
// and how many. Attribute i is the vertex shader's input at location i + 2, TEXCOORD i to xost, and the interpolator
// at location i.
struct XsdkAttribute {
    uint32_t offsetFloats;
    uint32_t floats;
};

// A combiner's program in the GLSL glslang takes for Vulkan, and the vertex it reads.
struct XsdkGlsl {
    std::string vertex;
    std::string fragment;
    // The position's 4 floats, then these.
    std::vector<XsdkAttribute> attributes;
    uint32_t vertexFloats;
};

// gfx_opengl's template's logic for xost (xenos-toolchain/docs/xsc.md): one uniform block, set 0 binding 0, holding
// what gfx_opengl passes as uniforms, the masks' sizes among them as xost has no textureSize; sampler2Ds at bindings 1
// to 6 for Fast3D's textures 0 to 5; no function calls.
XsdkGlsl BuildXsdkGlsl(const CCFeatures& features, bool threePointFiltering, bool srgbMode);

// The block's registers: each texture's size and its mask's, then three-point filtering, frame_count and noise_scale.
constexpr uint32_t XSDK_GLSL_BLOCK_FLOATS = 12;

// The program that clears: a position at location 0, and opaque black.
extern const char* const XSDK_GLSL_CLEAR_VERTEX;
extern const char* const XSDK_GLSL_CLEAR_FRAGMENT;

// The templates, which name the folder compiled programs are kept in.
std::string XsdkGlslTemplates();
} // namespace Fast
