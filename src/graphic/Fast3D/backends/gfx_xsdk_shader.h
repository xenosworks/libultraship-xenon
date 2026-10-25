#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Fast {
// A literal xost reads from a float constant, as Xenos has no immediates (xenos-toolchain/docs/xsc.md, "Literal
// constant pool").
struct XsdkLiteral {
    uint32_t reg;
    float value[4];
};

// What a program needs besides its microcode.
struct XsdkProgramLayout {
    // The pixel shader's uniform block, set 0 binding 0: its first float constant and how many, or -1 and 0.
    int32_t block;
    uint32_t blockRegisters;
    // The fetch constant of the pixel shader's sampler at binding i + 1, or -1 where it has none.
    int8_t samplers[8];
};

// A program's two shaders as xost compiled them.
struct XsdkCompiledProgram {
    std::vector<uint32_t> vertex;
    std::vector<uint32_t> pixel;
    std::vector<XsdkLiteral> vertexLiterals;
    std::vector<XsdkLiteral> pixelLiterals;
    XsdkProgramLayout layout;
    // Read from the disk or the game's shader pack, not compiled; from the pack; and the time spent in glslang and in
    // xost, compiled.
    bool read;
    bool packed;
    double glslangMs;
    double xostMs;
};

// Names the folder programs are kept in under root, after a hash of templates and glslang's and xost's versions, so
// programs from other templates or compilers aren't read back; returns it, or empty if it can't be made, and then
// nothing is kept.
std::string XsdkShaderCacheOpen(const std::string& root, const std::string& templates);

// The program named name, from the game's shader pack (gfx_xsdk_pack.h) if it holds it for this folder, else read
// from the folder, else compiled from its GLSL by glslang and xost and kept there; false
// with why in error where it can't be had, and true with error set where it was had but not kept. Its uniform block,
// if any, is to take blockRegisters registers.
bool XsdkShaderProgram(const std::string& folder, const std::string& name, const std::string& vertexGlsl,
                       const std::string& fragmentGlsl, uint32_t blockRegisters, XsdkCompiledProgram& program,
                       std::string& error);
} // namespace Fast
