#include "gfx_xsdk.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>

#include "../interpreter.h"
#include "gfx_xsdk_glsl.h"
#include "gfx_xsdk_window.h"
#include "public/bridge/consolevariablebridge.h"
#include "xsdk/log.h"
#ifndef XSDK_FAST3D_NULL
#include "gfx_xsdk_shader.h"
#endif

namespace Fast {
#ifdef XSDK_FAST3D_NULL
static constexpr bool DRAWS = false;
#else
static constexpr bool DRAWS = true;
#endif

// The time base: 49.875 ticks a microsecond (xsdk:src/time.c:22-26), its low word wrapping every 86 s.
static constexpr uint32_t TICKS_PER_SECOND = 49875000;
static constexpr uint32_t WINDOW_TICKS = 10 * TICKS_PER_SECOND;
// The GPU's clock (xsdk:include/xsdk/gpu.h).
static constexpr double GPU_TICKS_PER_MS = 498750.0;
// The interpreter allocates this squared at 4 bytes a texel for its uploads (interpreter.cpp:4198-4200): 16 MB, where
// 4096 took 64.
static constexpr int MAX_TEXTURE_SIZE = 2048;
// The Guide's first opening held a frame up 2.7 s (xsdk:docs/HARDWARE.md, "The Guide, milestone 37").
static constexpr uint32_t FENCE_TIMEOUT_MS = 5000;
// A draw's vertices are at most 256 triangles of 48 floats (interpreter.cpp:105, 1617-1762), 147 KB.
static constexpr uint32_t RING_SEGMENTS = 8;
static constexpr uint32_t SEGMENT_BYTES = 512 * 1024;
static constexpr uint32_t RING_BYTES = RING_SEGMENTS * SEGMENT_BYTES;
static constexpr uint32_t CLOCK_SLOTS = 4;
static constexpr uint32_t CLOCK_SLOT_BYTES = 32;
// Textures share 1 MB blocks in 4 KB pages, the alignment xsdk_texture_attach takes (xsdk:include/xsdk/texture.h).
static constexpr uint32_t PAGE_BYTES = 4096;
static constexpr uint32_t BLOCK_PAGES = 256;
static constexpr uint32_t BLOCK_REGISTERS = XSDK_GLSL_BLOCK_FLOATS / 4;
static constexpr uint32_t SHADER_TEXTURES = 6;
// GL's polygon offset of -2 units for decals (gfx_opengl.cpp:639), a unit the 24-bit depth's step, in xsdk's units of
// depth from 0 to 1 (xenia:src/xenia/gpu/draw_util.h:206-225).
static constexpr float DECAL_DEPTH_BIAS = -2.0f / 16777216.0f;

enum : uint32_t {
    DIRTY_DEPTH = 1 << 0,
    DIRTY_BLEND = 1 << 1,
    DIRTY_RASTER = 1 << 2,
    DIRTY_VIEWPORT = 1 << 3,
    // What xsdk_gpu_set_target sets back (xsdk:include/xsdk/gpu.h).
    DIRTY_TARGET = DIRTY_DEPTH | DIRTY_BLEND | DIRTY_RASTER | DIRTY_VIEWPORT,
};

static uint32_t Ticks() {
    uint32_t ticks;
    __asm__ volatile("mftb %0" : "=r"(ticks));
    return ticks;
}

static double Milliseconds(uint64_t ticks, uint32_t count) {
    return count == 0 ? 0.0 : ticks * 1000.0 / TICKS_PER_SECOND / count;
}

GfxRenderingAPIXsdk::Timed::Timed(Counts& counts) : counts(counts), start(Ticks()) {
}

GfxRenderingAPIXsdk::Timed::~Timed() {
    counts.backendTicks += Ticks() - start;
}

GfxRenderingAPIXsdk::GfxRenderingAPIXsdk(GfxWindowBackendXsdk* window) : mWindow(window) {
    window->SetRenderer(this);
    // Texture 0, black, and framebuffer 0, the screen, before anything asks for more.
    mTextures.resize(1);
    mFramebuffers.resize(1);
    mFramebuffers[0].fits = true;
}

void GfxRenderingAPIXsdk::LogWindow(uint32_t now) {
    const Counts& c = mCounts;
    const double seconds = (double)(now - mWindowStart) / TICKS_PER_SECOND;
    const uint32_t frames = c.frames == 0 ? 1 : c.frames;
    xsdk_log_write("gfx_xsdk: %u frames in %.1f s (%.1f fps); a frame: %.1f draws (%.1f skipped), %.0f triangles, %.0f "
                   "floats, %.3f ms in the backend (%.3f copying vertices), %.3f ms from StartFrame to EndFrame, "
                   "%.3f ms of GPU time over %u frames",
                   (unsigned)c.frames, seconds, c.frames / seconds, (double)c.draws / frames,
                   (double)c.skippedDraws / frames, (double)c.triangles / frames, (double)c.floats / frames,
                   Milliseconds(c.backendTicks, frames), Milliseconds(c.copyTicks, frames),
                   Milliseconds(c.frameTicks, frames),
                   c.gpuFrames == 0 ? 0.0 : c.gpuTicks / GPU_TICKS_PER_MS / c.gpuFrames, (unsigned)c.gpuFrames);
    xsdk_log_write(
        "gfx_xsdk: in the window: %u shaders created (%u compiled, %u read), %u loaded, %u kept; %u textures "
        "created, %u uploads of %llu KB; %u framebuffer calls",
        (unsigned)c.shadersCreated, (unsigned)c.shadersCompiled, (unsigned)c.shadersRead, (unsigned)c.shadersLoaded,
        (unsigned)mShaders.size(), (unsigned)c.texturesCreated, (unsigned)c.uploads,
        (unsigned long long)(c.uploadBytes / 1024), (unsigned)c.framebufferCalls);
    mCounts = {};
    mWindowStart = now;
}

void GfxRenderingAPIXsdk::LogOnce(bool& logged, const char* what) {
    if (!logged) {
        xsdk_log_write("gfx_xsdk: %s (logged once)", what);
        logged = true;
    }
}

bool GfxRenderingAPIXsdk::WaitFence(uint32_t fence, const char* what) {
    if (fence == 0) {
        return true;
    }
    const NTSTATUS status = xsdk_gpu_wait(fence, FENCE_TIMEOUT_MS);
    if (!NT_SUCCESS(status)) {
        xsdk_log_write("gfx_xsdk: xsdk_gpu_wait for %s failed: 0x%08X", what, (unsigned)status);
    }
    return NT_SUCCESS(status);
}

// Memory for texels: whole pages of a block with room, or an allocation of its own past a block.
GfxRenderingAPIXsdk::Memory GfxRenderingAPIXsdk::Allocate(uint32_t bytes) {
    const uint32_t pages = (bytes + PAGE_BYTES - 1) / PAGE_BYTES;
    if (pages > BLOCK_PAGES) {
        return { xsdk_gpu_alloc(bytes), -1, 0, 0 };
    }
    for (size_t b = 0;; b++) {
        if (b == mBlocks.size()) {
            volatile uint32_t* const memory = xsdk_gpu_alloc(BLOCK_PAGES * PAGE_BYTES);
            if (memory == nullptr) {
                return {};
            }
            mBlocks.push_back({ memory, {} });
        }
        PageBlock& block = mBlocks[b];
        uint32_t run = 0;
        for (uint32_t page = 0; page < BLOCK_PAGES; page++) {
            run = block.used[page] ? 0 : run + 1;
            if (run == pages) {
                const uint32_t first = page + 1 - pages;
                for (uint32_t p = first; p <= page; p++) {
                    block.used[p] = true;
                }
                volatile uint32_t* const texels = block.memory + first * PAGE_BYTES / 4;
                return { texels, (int32_t)b, first, pages };
            }
        }
    }
}

void GfxRenderingAPIXsdk::FreeNow(Memory& memory) {
    if (memory.texels == nullptr) {
        return;
    }
    if (memory.block < 0) {
        xsdk_gpu_free(memory.texels);
    } else {
        for (uint32_t p = memory.firstPage; p < memory.firstPage + memory.pages; p++) {
            mBlocks[memory.block].used[p] = false;
        }
    }
    memory = {};
}

// Draws queued this frame may still read it, so it goes once the GPU is past the frame's fence.
void GfxRenderingAPIXsdk::FreeAfterFrame(Memory& memory) {
    if (memory.texels != nullptr) {
        mPendingFrees.push_back({ memory, 0 });
    }
    memory = {};
}

void GfxRenderingAPIXsdk::ReleasePassed() {
    auto passed = [this](PendingFree& pending) {
        if (pending.fence == 0 || !xsdk_gpu_fence_done(pending.fence)) {
            return false;
        }
        FreeNow(pending.memory);
        return true;
    };
    mPendingFrees.erase(std::remove_if(mPendingFrees.begin(), mPendingFrees.end(), passed), mPendingFrees.end());
}

// New memory for a texture of this size, tiled R8G8B8A8, whose bytes are red, green, blue then alpha as Fast3D's
// uploads are (xsdk:include/xsdk/texture.h:55); its old memory goes after the frame.
bool GfxRenderingAPIXsdk::ReplaceTexels(Texture& texture, uint32_t width, uint32_t height, bool framebuffer) {
    XsdkTexture layout;
    NTSTATUS status = xsdk_texture_init(&layout, XSDK_FORMAT_R8G8B8A8, width, height, 1, true);
    if (!NT_SUCCESS(status)) {
        xsdk_log_write("gfx_xsdk: xsdk_texture_init(%ux%u) failed: 0x%08X", (unsigned)width, (unsigned)height,
                       (unsigned)status);
        return false;
    }
    if (framebuffer) {
        // A resolve writes from the corner (xsdk:include/xsdk/texture.h), and the framebuffer's alpha reads 1, as
        // gfx_opengl's RGB8 colour buffer's does (gfx_opengl.cpp:751).
        xsdk_texture_base_at_corner(&layout);
        static const uint32_t channels[4] = { XSDK_CHANNEL_RED, XSDK_CHANNEL_GREEN, XSDK_CHANNEL_BLUE,
                                              XSDK_CHANNEL_ONE };
        xsdk_texture_swizzle(&layout, channels);
    }
    const Memory memory = Allocate(layout.size_bytes);
    if (memory.texels == nullptr) {
        xsdk_log_write("gfx_xsdk: no GPU memory for a %ux%u texture, %u bytes", (unsigned)width, (unsigned)height,
                       (unsigned)layout.size_bytes);
        return false;
    }
    FreeAfterFrame(texture.memory);
    xsdk_texture_attach(&layout, memory.texels);
    texture.texture = layout;
    texture.memory = memory;
    texture.generation++;
    return true;
}

const GfxRenderingAPIXsdk::Texture& GfxRenderingAPIXsdk::TextureFor(uint32_t id) const {
    return id < mTextures.size() && mTextures[id].memory.texels != nullptr ? mTextures[id] : mTextures[0];
}

const char* GfxRenderingAPIXsdk::GetName() {
    return "xsdk";
}

int GfxRenderingAPIXsdk::GetMaxTextureSize() {
    return MAX_TEXTURE_SIZE;
}

GfxClipParameters GfxRenderingAPIXsdk::GetClipParameters() {
    // Xenos takes D3D's clip space, z from 0 to 1 and y up, as the D3D11 backend gives it (gfx_direct3d11.cpp:294-296).
    return { true, false };
}

void GfxRenderingAPIXsdk::UnloadShader(ShaderProgram* oldPrg) {
    Timed timed(mCounts);
}

void GfxRenderingAPIXsdk::LoadShader(ShaderProgram* newPrg) {
    Timed timed(mCounts);
    mCounts.shadersLoaded++;
    mShader = newPrg;
}

// The pixel shader's literals run from the register past its block, so they're set again with the shader, the block
// when it changes; the vertex shader's position and attributes go as they come (gfx_xsdk_glsl.h).
bool GfxRenderingAPIXsdk::FinishProgram(ShaderProgram& prg, const std::string& name, const std::string& vertexGlsl,
                                        const std::string& fragmentGlsl, const XsdkVertexElement* elements,
                                        uint32_t elementCount) {
    prg.drawable = false;
    prg.block = -1;
    memset(prg.fetchSlots, -1, sizeof(prg.fetchSlots));
#ifndef XSDK_FAST3D_NULL
    XsdkCompiledProgram compiled;
    std::string error;
    if (!XsdkShaderProgram(mShaderFolder, name, vertexGlsl, fragmentGlsl, BLOCK_REGISTERS, compiled, error)) {
        xsdk_log_write("gfx_xsdk: program %s isn't drawn: %s", name.c_str(), error.c_str());
        return false;
    }
    if (!error.empty()) {
        xsdk_log_write("gfx_xsdk: %s", error.c_str());
    }
    if (compiled.read) {
        mCounts.shadersRead++;
    } else {
        mCounts.shadersCompiled++;
        xsdk_log_write("gfx_xsdk: compiled program %s: glslang %.1f ms, xost %.1f ms", name.c_str(), compiled.glslangMs,
                       compiled.xostMs);
    }
    prg.vertexContainer = std::move(compiled.vertex);
    prg.pixelContainer = std::move(compiled.pixel);
    const uint32_t stride = prg.vertexFloats * 4;
    NTSTATUS status =
        xsdk_shader_load(&prg.vertex, prg.vertexContainer.data(), (uint32_t)prg.vertexContainer.size() * 4);
    if (NT_SUCCESS(status)) {
        status = xsdk_shader_load(&prg.pixel, prg.pixelContainer.data(), (uint32_t)prg.pixelContainer.size() * 4);
        if (!NT_SUCCESS(status)) {
            xsdk_shader_free(&prg.vertex);
        }
    }
    if (NT_SUCCESS(status)) {
        status = xsdk_shader_set_vertex_layout(&prg.vertex, elements, elementCount, &stride);
        // GLSL's stages meet by location; this refuses a pair that doesn't (xsdk:include/xsdk/shader.h).
        status = NT_SUCCESS(status) ? xsdk_shader_link(&prg.vertex, &prg.pixel) : status;
        if (!NT_SUCCESS(status)) {
            xsdk_shader_free(&prg.pixel);
            xsdk_shader_free(&prg.vertex);
        }
    }
    if (!NT_SUCCESS(status)) {
        xsdk_log_write("gfx_xsdk: loading program %s failed: 0x%08X", name.c_str(), (unsigned)status);
        return false;
    }
    auto runs = [](std::vector<XsdkLiteral> literals, std::vector<XsdkConstantRun>& out) {
        std::sort(literals.begin(), literals.end(),
                  [](const XsdkLiteral& a, const XsdkLiteral& b) { return a.reg < b.reg; });
        for (const XsdkLiteral& literal : literals) {
            if (out.empty() || out.back().first + out.back().count != literal.reg) {
                out.push_back({ literal.reg, 0, {} });
            }
            out.back().count++;
            out.back().values.insert(out.back().values.end(), literal.value, literal.value + 4);
        }
    };
    runs(compiled.vertexLiterals, prg.vertexLiterals);
    runs(compiled.pixelLiterals, prg.pixelLiterals);
    prg.block = compiled.layout.block;
    for (uint32_t t = 0; t < SHADER_TEXTURES; t++) {
        prg.fetchSlots[t] = compiled.layout.samplers[t];
    }
    prg.drawable = true;
#endif
    return prg.drawable;
}

void GfxRenderingAPIXsdk::LoadProgram(ShaderProgram& prg, const CCFeatures& features, uint64_t id0, uint32_t id1) {
    const bool threePoint = mFilter == FILTER_THREE_POINT;
    char name[40];
    snprintf(name, sizeof(name), "%016llX-%08X-%X", (unsigned long long)id0, (unsigned)id1,
             (threePoint ? 1u : 0u) | (mSrgbMode ? 2u : 0u));
    XsdkGlsl glsl;
    try {
        glsl = BuildXsdkGlsl(features, threePoint, mSrgbMode);
    } catch (const std::exception& e) {
        xsdk_log_write("gfx_xsdk: program %s isn't drawn: prism: %s", name, e.what());
        prg.drawable = false;
        return;
    }
    // Location 0 the position, then TEXCOORD i at location i + 2 (xenos-toolchain/docs/xsc.md, "Interface
    // conventions").
    std::vector<XsdkVertexElement> elements(glsl.attributes.size() + 1);
    elements[0] = { XSDK_USAGE_POSITION, 0, XSDK_VERTEX_FLOAT4, 0, 0, {}, false };
    for (size_t i = 0; i < glsl.attributes.size(); i++) {
        elements[i + 1] = { XSDK_USAGE_TEXCOORD,
                            (uint32_t)i,
                            XSDK_VERTEX_FLOAT1 + glsl.attributes[i].floats - 1,
                            0,
                            glsl.attributes[i].offsetFloats * 4,
                            {},
                            false };
    }
    prg.vertexFloats = glsl.vertexFloats;
    FinishProgram(prg, name, glsl.vertex, glsl.fragment, elements.data(), (uint32_t)elements.size());
}

ShaderProgram* GfxRenderingAPIXsdk::CreateAndLoadNewShader(uint64_t shaderId0, uint32_t shaderId1) {
    Timed timed(mCounts);
    CCFeatures features;
    gfx_cc_get_features(shaderId0, shaderId1, &features);
    ShaderProgram& prg = mShaders[std::make_pair(shaderId0, shaderId1)];
    prg.numInputs = features.numInputs;
    prg.usedTextures[0] = features.usedTextures[0];
    prg.usedTextures[1] = features.usedTextures[1];
    if (DRAWS) {
        LoadProgram(prg, features, shaderId0, shaderId1);
    }
    mCounts.shadersCreated++;
    mCounts.shadersLoaded++;
    mShader = &prg;
    return &prg;
}

ShaderProgram* GfxRenderingAPIXsdk::LookupShader(uint64_t shaderId0, uint32_t shaderId1) {
    Timed timed(mCounts);
    auto it = mShaders.find(std::make_pair(shaderId0, shaderId1));
    return it == mShaders.end() ? nullptr : &it->second;
}

void GfxRenderingAPIXsdk::ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) {
    Timed timed(mCounts);
    *numInputs = prg->numInputs;
    usedTextures[0] = prg->usedTextures[0];
    usedTextures[1] = prg->usedTextures[1];
}

uint32_t GfxRenderingAPIXsdk::NewTexture() {
    Timed timed(mCounts);
    mCounts.texturesCreated++;
    mTextures.emplace_back();
    return (uint32_t)mTextures.size() - 1;
}

void GfxRenderingAPIXsdk::SelectTexture(int tile, uint32_t textureId) {
    Timed timed(mCounts);
    mCurrentTile = tile;
    mCurrentTextureIds[tile] = textureId;
}

void GfxRenderingAPIXsdk::UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) {
    Timed timed(mCounts);
    mCounts.uploads++;
    mCounts.uploadBytes += (uint64_t)width * height * 4;
    const uint32_t id = mCurrentTextureIds[mCurrentTile];
    if (!DRAWS || id == 0 || id >= mTextures.size() || width == 0 || height == 0) {
        return;
    }
    // Draws queued since its last upload may still read it, so it takes new memory (xsdk:include/xsdk/texture.h:179).
    Texture& texture = mTextures[id];
    if (!ReplaceTexels(texture, width, height, false)) {
        return;
    }
    xsdk_texture_upload(&texture.texture, 0, rgba32Buf, width * 4);
    xsdk_gpu_invalidate(texture.texture.address, texture.texture.size_bytes);
}

// gfx_opengl's wrap modes (gfx_opengl.cpp:548-560): GL_MIRROR_CLAMP_TO_EDGE is Xenos's mirrored once.
static uint32_t AddressMode(uint32_t cm) {
    switch (cm & (G_TX_MIRROR | G_TX_CLAMP)) {
        case G_TX_NOMIRROR | G_TX_CLAMP:
            return XSDK_ADDRESS_CLAMP;
        case G_TX_MIRROR | G_TX_WRAP:
            return XSDK_ADDRESS_MIRROR;
        case G_TX_MIRROR | G_TX_CLAMP:
            return XSDK_ADDRESS_MIRROR_ONCE;
        default:
            return XSDK_ADDRESS_WRAP;
    }
}

// Per texture, as GL keeps a texture's parameters with it: the interpreter sets them only when a texture's own change
// (interpreter.cpp, the sampler parameters it keeps in each texture cache node).
void GfxRenderingAPIXsdk::SetSamplerParameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) {
    Timed timed(mCounts);
    const uint32_t id = mCurrentTextureIds[sampler];
    if (!DRAWS || id == 0 || id >= mTextures.size()) {
        return;
    }
    Texture& texture = mTextures[id];
    // Three-point filtering takes the place of the linear filter (gfx_opengl.cpp:564-567).
    const uint32_t filter = linear_filter && mFilter == FILTER_LINEAR ? XSDK_FILTER_LINEAR : XSDK_FILTER_POINT;
    XsdkSampler setting = {};
    setting.mag_filter = filter;
    setting.min_filter = filter;
    setting.mip_filter = XSDK_FILTER_POINT;
    setting.address_u = AddressMode(cms);
    setting.address_v = AddressMode(cmt);
    setting.address_w = XSDK_ADDRESS_WRAP;
    xsdk_texture_sampler_state(&texture.sampler, &setting);
    texture.threePoint = linear_filter;
    texture.generation++;
}

void GfxRenderingAPIXsdk::SetDepthTestAndMask(bool depth_test, bool z_upd) {
    Timed timed(mCounts);
    mCurrentDepthTest = depth_test;
    mCurrentDepthMask = z_upd;
    mDirty |= DIRTY_DEPTH;
}

void GfxRenderingAPIXsdk::SetZmodeDecal(bool decal) {
    Timed timed(mCounts);
    mCurrentZmodeDecal = decal;
    mDirty |= DIRTY_DEPTH | DIRTY_RASTER;
}

void GfxRenderingAPIXsdk::SetViewport(int x, int y, int width, int height) {
    Timed timed(mCounts);
    mViewport = { x, y, width, height };
    mDirty |= DIRTY_VIEWPORT;
}

void GfxRenderingAPIXsdk::SetScissor(int x, int y, int width, int height) {
    Timed timed(mCounts);
    mScissor = { x, y, width, height };
    mDirty |= DIRTY_VIEWPORT;
}

void GfxRenderingAPIXsdk::SetUseAlpha(bool useAlpha) {
    Timed timed(mCounts);
    mUseAlpha = useAlpha;
    mDirty |= DIRTY_BLEND;
}

// Depth as gfx_opengl sets it (gfx_opengl.cpp:598-645), its decal compare taken whenever decals change; GL's lower-left
// viewport and scissor as xsdk's rows from the top, its pixel centres on halves where xsdk's are whole.
void GfxRenderingAPIXsdk::ApplyState() {
    const Framebuffer& fb = mFramebuffers[mCurrentFb];
    if (mDirty & DIRTY_DEPTH) {
        xsdk_gpu_set_depth_stencil(
            &mDepthStates[(mCurrentDepthTest ? 1 : 0) | (mCurrentDepthMask ? 2 : 0) | (mCurrentZmodeDecal ? 4 : 0)]);
    }
    if (mDirty & DIRTY_RASTER) {
        if (mCurrentZmodeDecal) {
            // gfx_opengl's slope for the z-fighting setting: 120 gives -2 at 240 rows, as the N64 has it.
            float slope = -2.0f;
            switch (CVarGetInteger(CVAR_Z_FIGHTING_MODE, 0)) {
                case 1:
                    slope = -(float)fb.height / 120.0f;
                    break;
                case 2:
                    slope = -(float)fb.height / 100.0f;
                    break;
            }
            XsdkRaster raster = {};
            raster.cull = XSDK_CULL_NONE;
            raster.fill = XSDK_FILL_SOLID;
            raster.depth_bias = DECAL_DEPTH_BIAS;
            raster.depth_bias_slope = slope;
            raster.depth_clamp = true;
            XsdkRasterState state;
            xsdk_gpu_raster_state(&state, &raster);
            xsdk_gpu_set_raster(&state);
        } else {
            xsdk_gpu_set_raster(&mRasterPlain);
        }
    }
    if (mDirty & DIRTY_BLEND) {
        xsdk_gpu_set_blend(mUseAlpha ? &mBlendAlpha : &mBlendOpaque);
    }
    if (mDirty & DIRTY_VIEWPORT) {
        const int height = (int)fb.height;
        const int top = height - mViewport.y - mViewport.height;
        // Pixel centres on whole numbers, D3D9's, so GL's half a pixel up and left (vulkan4xenon:src/queue.c:151-160).
        xsdk_gpu_set_viewport((float)mViewport.x - 0.5f, (float)top - 0.5f, (float)mViewport.width,
                              (float)mViewport.height, 0.0f, 1.0f);
        // Depth clamp turns clipping off, and Xenos draws past the viewport as far as the scissor
        // (vulkan4xenon:src/queue.c:152-153), so the scissor stops at the viewport, as GL's clipping would.
        const int left = std::max({ mScissor.x, mViewport.x, 0 });
        const int right = std::min({ mScissor.x + mScissor.width, mViewport.x + mViewport.width, (int)fb.width });
        const int upper = std::max({ height - mScissor.y - mScissor.height, top, 0 });
        const int lower = std::min({ height - mScissor.y, top + mViewport.height, height });
        xsdk_gpu_set_scissor((uint32_t)std::min(left, (int)fb.width), (uint32_t)std::min(upper, height),
                             (uint32_t)std::max(right - left, 0), (uint32_t)std::max(lower - upper, 0));
    }
    mDirty = 0;
}

// The literals after each shader change, as another program's may sit in the same constants (xsdk:docs/GUIDE.md,
// "Shaders and their constants").
void GfxRenderingAPIXsdk::ApplyProgram(const ShaderProgram& prg) {
    if (&prg == mAppliedShader) {
        return;
    }
    xsdk_gpu_set_shaders(&prg.vertex, &prg.pixel);
    for (const XsdkConstantRun& run : prg.vertexLiterals) {
        xsdk_gpu_set_vertex_constants(run.first, run.values.data(), run.count);
    }
    for (const XsdkConstantRun& run : prg.pixelLiterals) {
        xsdk_gpu_set_pixel_constants(run.first, run.values.data(), run.count);
        const int32_t first = (int32_t)run.first;
        if (mBlockRegister >= 0 && first < mBlockRegister + (int32_t)BLOCK_REGISTERS &&
            first + (int32_t)run.count > mBlockRegister) {
            mBlockValid = false;
        }
    }
    mAppliedShader = &prg;
}

void GfxRenderingAPIXsdk::ApplyTextures(const ShaderProgram& prg) {
    for (uint32_t t = 0; t < SHADER_TEXTURES; t++) {
        const int slot = prg.fetchSlots[t];
        if (slot < 0) {
            continue;
        }
        const Texture& texture = TextureFor(mCurrentTextureIds[t]);
        FetchBinding& bound = mBound[slot];
        if (bound.texture != &texture || bound.generation != texture.generation) {
            xsdk_gpu_set_texture((uint32_t)slot, &texture.texture, &texture.sampler);
            bound = { &texture, texture.generation };
        }
    }
}

// gfx_opengl's uniforms (gfx_opengl.cpp:57-73), set when they change: the sizes of the textures the program samples,
// three-point filtering, frame_count and noise_scale.
void GfxRenderingAPIXsdk::ApplyBlock(const ShaderProgram& prg) {
    if (prg.block < 0) {
        return;
    }
    float block[XSDK_GLSL_BLOCK_FLOATS] = {};
    for (uint32_t i = 0; i < 2; i++) {
        if (prg.fetchSlots[i] >= 0) {
            const Texture& texture = TextureFor(mCurrentTextureIds[i]);
            block[i * 4 + 0] = (float)texture.texture.width;
            block[i * 4 + 1] = (float)texture.texture.height;
            block[8 + i] = texture.threePoint ? 1.0f : 0.0f;
        }
        if (prg.fetchSlots[SHADER_FIRST_MASK_TEXTURE + i] >= 0) {
            const Texture& mask = TextureFor(mCurrentTextureIds[SHADER_FIRST_MASK_TEXTURE + i]);
            block[i * 4 + 2] = (float)mask.texture.width;
            block[i * 4 + 3] = (float)mask.texture.height;
        }
    }
    block[10] = (float)mFrameCount;
    block[11] = mNoiseScale;
    if (mBlockValid && prg.block == mBlockRegister && memcmp(block, mBlock, sizeof(block)) == 0) {
        return;
    }
    xsdk_gpu_set_pixel_constants((uint32_t)prg.block, block, BLOCK_REGISTERS);
    memcpy(mBlock, block, sizeof(block));
    mBlockRegister = prg.block;
    mBlockValid = true;
}

void GfxRenderingAPIXsdk::NextSegment() {
    mRingFences[mRingSegment] = xsdk_gpu_fence();
    mRingSegment = (mRingSegment + 1) % RING_SEGMENTS;
    mRingOffset = 0;
    WaitFence(mRingFences[mRingSegment], "the vertex ring's next segment");
}

// xsdk's memcpy keeps to the stores write-combined memory takes (xsdk:docs/PITFALLS.md, "Plain stores to GPU memory");
// each draw's vertices start on 16 bytes.
uint32_t GfxRenderingAPIXsdk::WriteVertices(const float* floats, uint32_t bytes) {
    if (mRingOffset + bytes > SEGMENT_BYTES) {
        NextSegment();
    }
    const uint32_t offset = mRingSegment * SEGMENT_BYTES + mRingOffset;
    const uint32_t start = Ticks();
    memcpy((void*)((uintptr_t)mRing + offset), floats, bytes);
    mCounts.copyTicks += Ticks() - start;
    mRingOffset += (bytes + 15) & ~15u;
    return mRingAddress + offset;
}

void GfxRenderingAPIXsdk::DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    Timed timed(mCounts);
    mCounts.draws++;
    mCounts.triangles += buf_vbo_num_tris;
    mCounts.floats += buf_vbo_len;
    if (!DRAWS) {
        return;
    }
    const ShaderProgram* const prg = mShader;
    if (prg == nullptr || !prg->drawable || mTargetUndone) {
        mCounts.skippedDraws++;
        return;
    }
    // The vertices are as the program reads them, or nothing draws.
    if (buf_vbo_len != buf_vbo_num_tris * 3 * prg->vertexFloats) {
        LogOnce(mLoggedLayout, "a draw's vertices aren't the size its program reads; such draws are skipped");
        mCounts.skippedDraws++;
        return;
    }
    if (mDirty != 0) {
        ApplyState();
    }
    ApplyProgram(*prg);
    ApplyTextures(*prg);
    ApplyBlock(*prg);
    const uint32_t bytes = (uint32_t)buf_vbo_len * 4;
    const uint32_t address = WriteVertices(buf_vbo, bytes);
    // After the CPU writes vertices and before the draws that read them (xsdk:docs/PITFALLS.md, "Writes the GPU
    // doesn't see").
    xsdk_gpu_invalidate(address, bytes);
    xsdk_gpu_set_vertices(0, address, bytes);
    xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, (uint32_t)buf_vbo_num_tris * 3);
    mFramebuffers[mCurrentFb].drawn = true;
    mClearedFb = -1;
}

void GfxRenderingAPIXsdk::Init() {
    mWindowStart = Ticks();
    if (!DRAWS) {
        return;
    }
    mRing = xsdk_gpu_alloc(RING_BYTES + CLOCK_SLOTS * CLOCK_SLOT_BYTES);
    if (mRing == nullptr) {
        xsdk_log_write("gfx_xsdk: xsdk_gpu_alloc for the vertex ring failed; nothing draws");
        return;
    }
    mRingAddress = xsdk_gpu_address(mRing);
    // Texture 0, opaque black, which a texture that never got its texels samples as.
    static const uint8_t black[4] = { 0, 0, 0, 255 };
    if (ReplaceTexels(mTextures[0], 1, 1, false)) {
        xsdk_texture_upload(&mTextures[0].texture, 0, black, sizeof(black));
        xsdk_gpu_invalidate(mTextures[0].texture.address, mTextures[0].texture.size_bytes);
    }
    XsdkSampler point = {};
    xsdk_texture_sampler_state(&mTextures[0].sampler, &point);

    for (uint32_t i = 0; i < 8; i++) {
        const bool test = i & 1, mask = i & 2, decal = i & 4;
        XsdkDepthStencil depth = {};
        depth.depth_test = test || mask;
        depth.depth_write = mask;
        depth.depth_compare = !test ? XSDK_COMPARE_ALWAYS : decal ? XSDK_COMPARE_LESS_EQUAL : XSDK_COMPARE_LESS;
        xsdk_gpu_depth_stencil_state(&mDepthStates[i], &depth);
    }
    XsdkBlend blend = {};
    blend.color_source = XSDK_BLEND_ONE;
    blend.color_dest = XSDK_BLEND_ZERO;
    blend.alpha_source = XSDK_BLEND_ONE;
    blend.alpha_dest = XSDK_BLEND_ZERO;
    blend.write_mask = XSDK_WRITE_ALL;
    xsdk_gpu_blend_state(&mBlendOpaque, &blend);
    blend.write_mask = 0;
    xsdk_gpu_blend_state(&mBlendNoColor, &blend);
    // glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), for colour and alpha alike (gfx_opengl.cpp:671).
    blend.color_source = XSDK_BLEND_SRC_ALPHA;
    blend.color_dest = XSDK_BLEND_ONE_MINUS_SRC_ALPHA;
    blend.alpha_source = XSDK_BLEND_SRC_ALPHA;
    blend.alpha_dest = XSDK_BLEND_ONE_MINUS_SRC_ALPHA;
    blend.write_mask = XSDK_WRITE_ALL;
    xsdk_gpu_blend_state(&mBlendAlpha, &blend);
    // No culling, as Fast3D culls (interpreter.cpp:1362-1404), and depth clamp, GL_DEPTH_CLAMP (gfx_opengl.cpp:668).
    XsdkRaster raster = {};
    raster.cull = XSDK_CULL_NONE;
    raster.fill = XSDK_FILL_SOLID;
    raster.depth_clamp = true;
    xsdk_gpu_raster_state(&mRasterPlain, &raster);

#ifndef XSDK_FAST3D_NULL
    mShaderFolder = XsdkShaderCacheOpen("game:/shaders/fast3d", XsdkGlslTemplates());
    if (mShaderFolder.empty()) {
        xsdk_log_write("gfx_xsdk: game:/shaders/fast3d couldn't be made; programs compiled aren't kept");
    } else {
        xsdk_log_write("gfx_xsdk: programs are kept in %s", mShaderFolder.c_str());
    }
#endif
    const XsdkVertexElement position = { XSDK_USAGE_POSITION, 0, XSDK_VERTEX_FLOAT4, 0, 0, {}, false };
    mClearProgram.vertexFloats = 4;
    FinishProgram(mClearProgram, "clear", XSDK_GLSL_CLEAR_VERTEX, XSDK_GLSL_CLEAR_FRAGMENT, &position, 1);
}

void GfxRenderingAPIXsdk::Release() {
    if (!DRAWS || mRing == nullptr) {
        return;
    }
    // Everything below may be read until the GPU is past what's queued.
    if (!WaitFence(xsdk_gpu_fence(), "the GPU before releasing the backend")) {
        return;
    }
    for (auto& entry : mShaders) {
        if (entry.second.drawable) {
            xsdk_shader_free(&entry.second.pixel);
            xsdk_shader_free(&entry.second.vertex);
        }
    }
    mShaders.clear();
    if (mClearProgram.drawable) {
        xsdk_shader_free(&mClearProgram.pixel);
        xsdk_shader_free(&mClearProgram.vertex);
        mClearProgram.drawable = false;
    }
    for (PendingFree& pending : mPendingFrees) {
        FreeNow(pending.memory);
    }
    mPendingFrees.clear();
    for (Texture& texture : mTextures) {
        FreeNow(texture.memory);
    }
    for (PageBlock& block : mBlocks) {
        xsdk_gpu_free(block.memory);
    }
    mBlocks.clear();
    xsdk_gpu_free(mRing);
    mRing = nullptr;
}

void GfxRenderingAPIXsdk::OnResize() {
}

// Each frame's GPU time from the clock written as the command processor reached its start and once the GPU was idle
// after its present.
void GfxRenderingAPIXsdk::ReadClocks() {
    for (uint32_t slot = 0; slot < CLOCK_SLOTS; slot++) {
        ClockSlot& clock = mClocks[slot];
        if (clock.pending && xsdk_gpu_fence_done(clock.fence)) {
            volatile const uint32_t* const words = mRing + (RING_BYTES + slot * CLOCK_SLOT_BYTES) / 4;
            mCounts.gpuTicks += xsdk_gpu_clock_value(words + 4) - xsdk_gpu_clock_value(words);
            mCounts.gpuFrames++;
            clock.pending = false;
        }
    }
}

void GfxRenderingAPIXsdk::StartFrame() {
    Timed timed(mCounts);
    mFrameStart = timed.start;
    mFrameCount++;
    if (!DRAWS || mRing == nullptr) {
        return;
    }
    ReleasePassed();
    ReadClocks();
    const uint32_t slot = mFrameCount % CLOCK_SLOTS;
    mTimingFrame = !mClocks[slot].pending;
    if (mTimingFrame) {
        xsdk_gpu_write_clock(mRingAddress + RING_BYTES + slot * CLOCK_SLOT_BYTES, false);
    }
}

void GfxRenderingAPIXsdk::EndFrame() {
    const uint32_t now = Ticks();
    mCounts.frameTicks += now - mFrameStart;
    mCounts.frames++;
    if (now - mWindowStart >= WINDOW_TICKS) {
        LogWindow(now);
    }
    if (DRAWS && mCurrentFb != 0) {
        // The window resolves framebuffer 0 into the screen; Fast3D always ends there (interpreter.cpp:4335-4356).
        StartDrawToFramebuffer(0, 0.0f);
    }
}

// After the window's present, which resolved the screen and cleared it (gfx_xsdk_window.cpp, SwapBuffersBegin), and
// after ImGui, which set its own state.
void GfxRenderingAPIXsdk::FinishRender() {
    Timed timed(mCounts);
    if (!DRAWS || mRing == nullptr) {
        return;
    }
    Resolved();
    // ImGui's literals may sit in the block's constants.
    mBlockValid = false;
    // XAM's drawing under the Guide changes eDRAM between frames (xsdk:include/xsdk/gpu.h, XsdkGpuStats).
    XsdkGpuStats stats;
    xsdk_gpu_stats(&stats);
    mClearedFb = mWindow->Presented() && stats.system_draws == mSystemDraws ? 0 : -1;
    mSystemDraws = stats.system_draws;
    for (Framebuffer& fb : mFramebuffers) {
        fb.drawn = false;
    }
    const uint32_t slot = mFrameCount % CLOCK_SLOTS;
    if (mTimingFrame) {
        xsdk_gpu_write_clock(mRingAddress + RING_BYTES + slot * CLOCK_SLOT_BYTES + 16, true);
    }
    const uint32_t fence = xsdk_gpu_fence();
    if (mTimingFrame) {
        mClocks[slot] = { fence, true };
    }
    for (PendingFree& pending : mPendingFrees) {
        if (pending.fence == 0) {
            pending.fence = fence;
        }
    }
}

int GfxRenderingAPIXsdk::CreateFramebuffer() {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
    // 0 is the screen's (interpreter.cpp:4178).
    mFramebuffers.push_back({});
    mFramebuffers.back().texture = (uint32_t)mTextures.size();
    mTextures.emplace_back();
    return (int)mFramebuffers.size() - 1;
}

// One sample, and a target that fits in eDRAM at once, 1280x720 at most (xsdk:docs/scope/61-fast3d.md, Decisions, 6).
void GfxRenderingAPIXsdk::UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                                      bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                                      bool can_extract_depth) {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
    if (msaa_level > 1) {
        LogOnce(mLoggedMsaa, "multisampling isn't taken; framebuffers draw at 1 sample");
    }
    Framebuffer& fb = mFramebuffers[fb_id];
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (fb.width == width && fb.height == height) {
        return;
    }
    fb.width = width;
    fb.height = height;
    fb.fits = 2 * xsdk_gpu_target_tiles(width, height) <= XSDK_EDRAM_TILES;
    if (!fb.fits) {
        LogOnce(mLoggedTooBig, "a framebuffer past eDRAM's tiles isn't drawn");
    }
    if (DRAWS && fb_id != 0 && fb.fits) {
        fb.fits = ReplaceTexels(mTextures[fb.texture], width, height, true);
    }
    if (DRAWS && fb_id == mCurrentFb && !mTargetUndone) {
        SetTarget();
    }
}

// Draws after it need the target and the rest set again (xsdk:docs/GUIDE.md, "Textures, render targets and
// resolves").
void GfxRenderingAPIXsdk::Resolved() {
    mTargetUndone = true;
    mDirty = DIRTY_TARGET;
    mAppliedShader = nullptr;
    for (FetchBinding& bound : mBound) {
        bound = {};
    }
}

void GfxRenderingAPIXsdk::SetTarget() {
    const Framebuffer& fb = mFramebuffers[mCurrentFb];
    if (!fb.fits) {
        mTargetUndone = true;
        return;
    }
    xsdk_gpu_set_target(fb.width, fb.height);
    mTargetUndone = false;
    mDirty |= DIRTY_TARGET;
    if (mClearedFb != mCurrentFb) {
        mClearedFb = -1;
    }
}

// A framebuffer's colour goes into its texture as it's left, and the resolve clears its eDRAM.
// shortcut: what the screen holds is lost if another framebuffer is drawn before the frame ends, which Starship never
// does; draw it back from a texture of its own when a game that does is brought up.
void GfxRenderingAPIXsdk::LeaveFramebuffer() {
    Framebuffer& fb = mFramebuffers[mCurrentFb];
    if (mTargetUndone || !fb.drawn) {
        return;
    }
    if (mCurrentFb == 0) {
        LogOnce(mLoggedScreenLost,
                "the screen's drawing is lost: another framebuffer was drawn before the frame ended");
        return;
    }
    xsdk_gpu_resolve_texture(&mTextures[fb.texture].texture, XSDK_CLEAR_ARGB);
    Resolved();
    mClearedFb = mCurrentFb;
    fb.drawn = false;
}

void GfxRenderingAPIXsdk::StartDrawToFramebuffer(int fbId, float noiseScale) {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
    if (noiseScale != 0.0f) {
        mNoiseScale = 1.0f / noiseScale;
    }
    if (!DRAWS || mRing == nullptr || (fbId == mCurrentFb && !mTargetUndone)) {
        return;
    }
    if (fbId != mCurrentFb) {
        LeaveFramebuffer();
    }
    mCurrentFb = fbId;
    SetTarget();
}

void GfxRenderingAPIXsdk::CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1,
                                          int dstX0, int dstY0, int dstX1, int dstY1) {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
    LogOnce(mLoggedCopyFramebuffer, "CopyFramebuffer isn't implemented; it copies nothing");
}

// A triangle over the whole target at the far plane, as gfx_opengl clears with the scissor off
// (gfx_opengl.cpp:797-804); nothing where the last resolve left this target's eDRAM cleared already.
void GfxRenderingAPIXsdk::ClearFramebuffer(bool color, bool depth) {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
    if (!DRAWS || mTargetUndone || mClearedFb == mCurrentFb || !mClearProgram.drawable || (!color && !depth)) {
        return;
    }
    const Framebuffer& fb = mFramebuffers[mCurrentFb];
    static const float corners[12] = { -1.0f, -1.0f, 1.0f, 1.0f, 3.0f, -1.0f, 1.0f, 1.0f, -1.0f, 3.0f, 1.0f, 1.0f };
    xsdk_gpu_set_viewport(0.0f, 0.0f, (float)fb.width, (float)fb.height, 0.0f, 1.0f);
    xsdk_gpu_set_scissor(0, 0, fb.width, fb.height);
    // Depth tested ALWAYS and written, or not at all.
    xsdk_gpu_set_depth_stencil(&mDepthStates[depth ? 2 : 0]);
    xsdk_gpu_set_blend(color ? &mBlendOpaque : &mBlendNoColor);
    xsdk_gpu_set_raster(&mRasterPlain);
    ApplyProgram(mClearProgram);
    const uint32_t address = WriteVertices(corners, sizeof(corners));
    xsdk_gpu_invalidate(address, sizeof(corners));
    xsdk_gpu_set_vertices(0, address, sizeof(corners));
    xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, 3);
    mDirty |= DIRTY_TARGET;
    mFramebuffers[mCurrentFb].drawn = true;
    mClearedFb = color && depth ? mCurrentFb : -1;
}

void GfxRenderingAPIXsdk::ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
    LogOnce(mLoggedReadFramebuffer, "ReadFramebufferToCPU isn't implemented; it returns nothing");
    memset(rgba16Buf, 0, (size_t)width * height * sizeof(*rgba16Buf));
}

void GfxRenderingAPIXsdk::ResolveMSAAColorBuffer(int fbIdTarger, int fbIdSrc) {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
}

std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
GfxRenderingAPIXsdk::GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
    LogOnce(mLoggedPixelDepth, "GetPixelDepth isn't implemented; it returns nothing");
    return {};
}

// The framebuffer's texture, an XsdkTexture*, imgui_impl_xsdk's ImTextureID (imgui-xenon:backends/imgui_impl_xsdk.h).
void* GfxRenderingAPIXsdk::GetFramebufferTextureId(int fbId) {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
    if (!DRAWS || fbId <= 0 || fbId >= (int)mFramebuffers.size()) {
        return nullptr;
    }
    return GetTextureById((int)mFramebuffers[fbId].texture);
}

void GfxRenderingAPIXsdk::SelectTextureFb(int fbId) {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
    if (fbId > 0 && fbId < (int)mFramebuffers.size()) {
        mCurrentTile = 0;
        mCurrentTextureIds[0] = mFramebuffers[fbId].texture;
    }
}

void GfxRenderingAPIXsdk::DeleteTexture(uint32_t texId) {
    Timed timed(mCounts);
    if (DRAWS && texId != 0 && texId < mTextures.size()) {
        FreeAfterFrame(mTextures[texId].memory);
        mTextures[texId].texture = {};
        mTextures[texId].generation++;
    }
}

void GfxRenderingAPIXsdk::SetTextureFilter(FilteringMode mode) {
    // The OpenGL backend's: textures are made again under the new filter (gfx_opengl.cpp:977-980).
    gfx_texture_cache_clear();
    mFilter = mode;
}

FilteringMode GfxRenderingAPIXsdk::GetTextureFilter() {
    return mFilter;
}

void GfxRenderingAPIXsdk::SetSrgbMode() {
    mSrgbMode = true;
}

ImTextureID GfxRenderingAPIXsdk::GetTextureById(int id) {
    if (!DRAWS) {
        // Step 1's: ImGui draws nothing then, so nothing reads it.
        return reinterpret_cast<ImTextureID>(id);
    }
    if (id <= 0 || id >= (int)mTextures.size() || mTextures[id].memory.texels == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<ImTextureID>(&mTextures[id].texture);
}
} // namespace Fast
