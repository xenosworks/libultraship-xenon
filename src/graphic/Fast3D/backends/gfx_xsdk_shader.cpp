#include "gfx_xsdk_shader.h"

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <sys/stat.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "gfx_xsdk_pack.h"
#include "xsdk/xsc.h"

// A game without a pack: every program comes from the disk or glslang.
extern "C" __attribute__((weak)) const XsdkShaderPack xsdk_fast3d_pack{};

namespace Fast {
// "F3DX", and the layout of what follows it, raised as it changes.
static constexpr uint32_t CACHE_MAGIC = 0x46334458;
static constexpr uint32_t CACHE_LAYOUT = 1;
// No container xost writes is near this; a file that says so is damaged.
static constexpr uint32_t MAX_CACHED_BYTES = 1024 * 1024;
static constexpr uint32_t MAX_SAMPLER_BINDING = sizeof(XsdkProgramLayout::samplers);

// A kept program's file: this, the vertex and pixel containers, then their literals.
struct CacheHeader {
    uint32_t magic;
    uint32_t layout;
    uint32_t vertexBytes;
    uint32_t pixelBytes;
    uint32_t vertexLiterals;
    uint32_t pixelLiterals;
    XsdkProgramLayout program;
};

static uint64_t Fnv1a(uint64_t hash, const std::string& text) {
    for (unsigned char c : text) {
        hash = (hash ^ c) * 0x100000001B3ull;
    }
    return hash;
}

static double MillisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

static bool MakeFolder(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 || mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
}

std::string XsdkShaderCacheOpen(const std::string& root, const std::string& templates) {
    const glslang::Version version = glslang::GetVersion();
    char glslang[64];
    snprintf(glslang, sizeof(glslang), "glslang %d.%d.%d%s", version.major, version.minor, version.patch,
             version.flavor);
    const uint64_t hash = Fnv1a(Fnv1a(Fnv1a(0xCBF29CE484222325ull, templates), glslang), xsdk_xsc_version());
    char name[17];
    snprintf(name, sizeof(name), "%016llX", (unsigned long long)hash);
    const std::string folder = root + "/" + name;
    // Each folder on the way, past the drive.
    for (size_t at = folder.find('/'); at != std::string::npos; at = folder.find('/', at + 1)) {
        const std::string part = folder.substr(0, at);
        if (!part.empty() && part.back() != ':' && !MakeFolder(part)) {
            return {};
        }
    }
    return MakeFolder(folder) ? folder : std::string{};
}

// As `glslang -V` makes SPIR-V of GLSL, with the settings that gave the PC's SPIR-V on the console
// (aurora-xenon:lib/vulkan4xenon/program.cpp).
static bool CompileGlsl(const std::string& source, bool fragment, std::vector<uint32_t>& spirv, std::string& error) {
    static bool started = false;
    if (!started) {
        glslang::InitializeProcess();
        started = true;
    }
    const EShLanguage stage = fragment ? EShLangFragment : EShLangVertex;
    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    glslang::TShader shader(stage);
    const char* const text = source.c_str();
    shader.setStrings(&text, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
        error = std::string("glslang refused the ") + (fragment ? "fragment" : "vertex") +
                " shader: " + shader.getInfoLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        error = std::string("glslang didn't link the ") + (fragment ? "fragment" : "vertex") +
                " shader: " + program.getInfoLog();
        return false;
    }
    glslang::SpvOptions options;
    options.disableOptimizer = false;
    std::vector<unsigned int> words;
    glslang::GlslangToSpv(*program.getIntermediate(stage), words, &options);
    spirv.assign(words.begin(), words.end());
    return true;
}

// Through xost at xsdk's register split with the optimiser on, as vulkan4xenon compiles (vulkan4xenon:src/shader.c).
// Only literals are taken among its needs: the templates have no loops a block counts and no clip planes.
static bool CompileSpirv(const std::vector<uint32_t>& spirv, bool fragment, XsdkCompiledProgram& program,
                         std::string& error) {
    XsdkXscOptions options;
    xsdk_xsc_default_options(&options);
    options.optimize = true;
    XsdkXscShader compiled;
    const XsdkXscStatus status = xsdk_xsc_compile(spirv.data(), spirv.size(), &options, &compiled);
    const char* const stage = fragment ? "fragment" : "vertex";
    if (status != XSDK_XSC_OK) {
        error =
            std::string("xost refused the ") + stage + " shader (" + std::to_string(status) + "): " + compiled.message;
        return false;
    }
    bool taken = true;
    std::vector<XsdkLiteral>& literals = fragment ? program.pixelLiterals : program.vertexLiterals;
    for (size_t i = 0; i < compiled.need_count && taken; i++) {
        const XsdkXscNeed& need = compiled.needs[i];
        taken = need.kind == XSDK_XSC_LITERAL;
        const XsdkLiteral literal = {
            need.reg, { (float)need.value[0], (float)need.value[1], (float)need.value[2], (float)need.value[3] }
        };
        if (taken) {
            literals.push_back(literal);
        }
    }
    for (size_t i = 0; i < compiled.resource_count && taken; i++) {
        const XsdkXscResource& resource = compiled.resources[i];
        if (!fragment || resource.set != 0) {
            taken = false;
        } else if (resource.kind == XSDK_XSC_UNIFORM_BUFFER && resource.binding == 0) {
            program.layout.block = (int32_t)resource.reg;
            program.layout.blockRegisters = resource.regs;
        } else if (resource.kind == XSDK_XSC_SAMPLER && resource.binding >= 1 &&
                   resource.binding <= MAX_SAMPLER_BINDING) {
            program.layout.samplers[resource.binding - 1] = (int8_t)resource.reg;
        } else {
            taken = false;
        }
    }
    if (taken) {
        const uint32_t* const words = reinterpret_cast<const uint32_t*>(compiled.container);
        (fragment ? program.pixel : program.vertex).assign(words, words + compiled.container_bytes / 4);
    } else {
        error = std::string("the ") + stage + " shader needs what the backend doesn't set";
    }
    xsdk_xsc_free(&compiled);
    return taken;
}

// A kept program's bytes, from its file or the game's pack, taken only if they're all there and nothing more.
static bool ParseKept(const uint8_t* bytes, size_t size, XsdkCompiledProgram& program) {
    CacheHeader header;
    if (size < sizeof(header)) {
        return false;
    }
    memcpy(&header, bytes, sizeof(header));
    if (header.magic != CACHE_MAGIC || header.layout != CACHE_LAYOUT || header.vertexBytes >= MAX_CACHED_BYTES ||
        header.pixelBytes >= MAX_CACHED_BYTES || header.vertexLiterals >= MAX_CACHED_BYTES ||
        header.pixelLiterals >= MAX_CACHED_BYTES || header.vertexBytes % 4 != 0 || header.pixelBytes % 4 != 0 ||
        size != sizeof(header) + header.vertexBytes + header.pixelBytes +
                    (header.vertexLiterals + header.pixelLiterals) * sizeof(XsdkLiteral)) {
        return false;
    }
    program.vertex.resize(header.vertexBytes / 4);
    program.pixel.resize(header.pixelBytes / 4);
    program.vertexLiterals.resize(header.vertexLiterals);
    program.pixelLiterals.resize(header.pixelLiterals);
    program.layout = header.program;
    const uint8_t* at = bytes + sizeof(header);
    auto take = [&at](void* to, size_t count) {
        memcpy(to, at, count);
        at += count;
    };
    take(program.vertex.data(), header.vertexBytes);
    take(program.pixel.data(), header.pixelBytes);
    take(program.vertexLiterals.data(), program.vertexLiterals.size() * sizeof(XsdkLiteral));
    take(program.pixelLiterals.data(), program.pixelLiterals.size() * sizeof(XsdkLiteral));
    return true;
}

static bool ReadKept(const std::string& path, XsdkCompiledProgram& program) {
    FILE* const file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    std::vector<uint8_t> bytes;
    uint8_t chunk[4096];
    size_t got;
    // A file past the most a sound one holds is damaged, and isn't read to its end.
    while (bytes.size() <= MAX_CACHED_BYTES * 3 && (got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        bytes.insert(bytes.end(), chunk, chunk + got);
    }
    fclose(file);
    return ParseKept(bytes.data(), bytes.size(), program);
}

// The game's pack's program of this name, if the pack was made from this folder's templates and compilers.
static bool FromPack(const std::string& folder, const std::string& name, XsdkCompiledProgram& program) {
    const XsdkShaderPack& pack = xsdk_fast3d_pack;
    const size_t slash = folder.rfind('/');
    if (pack.folder == nullptr ||
        folder.compare(slash == std::string::npos ? 0 : slash + 1, std::string::npos, pack.folder) != 0) {
        return false;
    }
    for (uint32_t i = 0; i < pack.count; i++) {
        if (name == pack.programs[i].name) {
            return ParseKept(pack.programs[i].bytes, pack.programs[i].size, program);
        }
    }
    return false;
}

// Written beside its name and renamed into place, so a crash leaves no file half written.
static bool Keep(const std::string& path, const XsdkCompiledProgram& program) {
    const CacheHeader header = { CACHE_MAGIC,
                                 CACHE_LAYOUT,
                                 (uint32_t)program.vertex.size() * 4,
                                 (uint32_t)program.pixel.size() * 4,
                                 (uint32_t)program.vertexLiterals.size(),
                                 (uint32_t)program.pixelLiterals.size(),
                                 program.layout };
    const std::string written = path + ".new";
    FILE* const file = fopen(written.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    bool kept = fwrite(&header, sizeof(header), 1, file) == 1 &&
                fwrite(program.vertex.data(), 4, program.vertex.size(), file) == program.vertex.size() &&
                fwrite(program.pixel.data(), 4, program.pixel.size(), file) == program.pixel.size() &&
                fwrite(program.vertexLiterals.data(), sizeof(XsdkLiteral), program.vertexLiterals.size(), file) ==
                    program.vertexLiterals.size() &&
                fwrite(program.pixelLiterals.data(), sizeof(XsdkLiteral), program.pixelLiterals.size(), file) ==
                    program.pixelLiterals.size();
    kept = fclose(file) == 0 && kept;
    kept = kept && rename(written.c_str(), path.c_str()) == 0;
    if (!kept) {
        remove(written.c_str());
    }
    return kept;
}

bool XsdkShaderProgram(const std::string& folder, const std::string& name, const std::string& vertexGlsl,
                       const std::string& fragmentGlsl, uint32_t blockRegisters, XsdkCompiledProgram& program,
                       std::string& error) {
    program = {};
    program.layout.block = -1;
    memset(program.layout.samplers, -1, sizeof(program.layout.samplers));
    const std::string path = folder.empty() ? std::string{} : folder + "/" + name + ".f3dx";
    if (!folder.empty() && FromPack(folder, name, program)) {
        program.read = true;
        program.packed = true;
        return true;
    }
    program = {};
    program.layout.block = -1;
    memset(program.layout.samplers, -1, sizeof(program.layout.samplers));
    if (!path.empty() && ReadKept(path, program)) {
        program.read = true;
        return true;
    }
    program = {};
    program.layout.block = -1;
    memset(program.layout.samplers, -1, sizeof(program.layout.samplers));
    std::vector<uint32_t> vertexSpirv;
    std::vector<uint32_t> pixelSpirv;
    auto start = std::chrono::steady_clock::now();
    if (!CompileGlsl(vertexGlsl, false, vertexSpirv, error) || !CompileGlsl(fragmentGlsl, true, pixelSpirv, error)) {
        return false;
    }
    program.glslangMs = MillisecondsSince(start);
    start = std::chrono::steady_clock::now();
    if (!CompileSpirv(vertexSpirv, false, program, error) || !CompileSpirv(pixelSpirv, true, program, error)) {
        return false;
    }
    program.xostMs = MillisecondsSince(start);
    if (program.layout.block >= 0 && program.layout.blockRegisters != blockRegisters) {
        error = "the uniform block takes " + std::to_string(program.layout.blockRegisters) + " registers, not " +
                std::to_string(blockRegisters);
        return false;
    }
    if (!path.empty() && !Keep(path, program)) {
        error = path + " wasn't kept";
    }
    return true;
}
} // namespace Fast
