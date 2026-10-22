#include "gfx_xsdk.h"

#include <cstring>

#include "../interpreter.h"
#include "xsdk/log.h"

namespace Fast {
// The time base: 49.875 ticks a microsecond (xsdk:src/time.c:22-26), its low word wrapping every 86 s.
static constexpr uint32_t TICKS_PER_SECOND = 49875000;
static constexpr uint32_t WINDOW_TICKS = 10 * TICKS_PER_SECOND;

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

void GfxRenderingAPIXsdk::LogWindow(uint32_t now) {
    const Counts& c = mCounts;
    const double seconds = (double)(now - mWindowStart) / TICKS_PER_SECOND;
    const uint32_t frames = c.frames == 0 ? 1 : c.frames;
    xsdk_log_write("gfx_xsdk: %u frames in %.1f s (%.1f fps); a frame: %.1f draws, %.0f triangles, %.0f floats, "
                   "%.3f ms in the backend, %.3f ms from StartFrame to EndFrame",
                   (unsigned)c.frames, seconds, c.frames / seconds, (double)c.draws / frames,
                   (double)c.triangles / frames, (double)c.floats / frames, Milliseconds(c.backendTicks, frames),
                   Milliseconds(c.frameTicks, frames));
    xsdk_log_write(
        "gfx_xsdk: in the window: %u shaders created, %u loaded, %u kept; %u textures created, %u uploads of "
        "%llu KB; %u framebuffer calls",
        (unsigned)c.shadersCreated, (unsigned)c.shadersLoaded, (unsigned)mShaders.size(), (unsigned)c.texturesCreated,
        (unsigned)c.uploads, (unsigned long long)(c.uploadBytes / 1024), (unsigned)c.framebufferCalls);
    mCounts = {};
    mWindowStart = now;
}

void GfxRenderingAPIXsdk::LogFirstCall(bool& logged, const char* name) {
    if (!logged) {
        xsdk_log_write("gfx_xsdk: %s isn't implemented; it returns nothing (logged once)", name);
        logged = true;
    }
}

const char* GfxRenderingAPIXsdk::GetName() {
    return "xsdk";
}

int GfxRenderingAPIXsdk::GetMaxTextureSize() {
    return 4096;
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
}

ShaderProgram* GfxRenderingAPIXsdk::CreateAndLoadNewShader(uint64_t shaderId0, uint32_t shaderId1) {
    Timed timed(mCounts);
    CCFeatures features;
    gfx_cc_get_features(shaderId0, shaderId1, &features);
    ShaderProgram& prg = mShaders[std::make_pair(shaderId0, shaderId1)];
    prg.numInputs = features.numInputs;
    prg.usedTextures[0] = features.usedTextures[0];
    prg.usedTextures[1] = features.usedTextures[1];
    mCounts.shadersCreated++;
    mCounts.shadersLoaded++;
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
    return ++mLastTextureId;
}

void GfxRenderingAPIXsdk::SelectTexture(int tile, uint32_t textureId) {
    Timed timed(mCounts);
}

void GfxRenderingAPIXsdk::UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) {
    Timed timed(mCounts);
    mCounts.uploads++;
    mCounts.uploadBytes += (uint64_t)width * height * 4;
}

void GfxRenderingAPIXsdk::SetSamplerParameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) {
    Timed timed(mCounts);
}

void GfxRenderingAPIXsdk::SetDepthTestAndMask(bool depth_test, bool z_upd) {
    Timed timed(mCounts);
}

void GfxRenderingAPIXsdk::SetZmodeDecal(bool decal) {
    Timed timed(mCounts);
}

void GfxRenderingAPIXsdk::SetViewport(int x, int y, int width, int height) {
    Timed timed(mCounts);
}

void GfxRenderingAPIXsdk::SetScissor(int x, int y, int width, int height) {
    Timed timed(mCounts);
}

void GfxRenderingAPIXsdk::SetUseAlpha(bool useAlpha) {
    Timed timed(mCounts);
}

void GfxRenderingAPIXsdk::DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    Timed timed(mCounts);
    mCounts.draws++;
    mCounts.triangles += buf_vbo_num_tris;
    mCounts.floats += buf_vbo_len;
}

void GfxRenderingAPIXsdk::Init() {
    mWindowStart = Ticks();
}

void GfxRenderingAPIXsdk::OnResize() {
}

void GfxRenderingAPIXsdk::StartFrame() {
    Timed timed(mCounts);
    mFrameStart = timed.start;
}

void GfxRenderingAPIXsdk::EndFrame() {
    const uint32_t now = Ticks();
    mCounts.frameTicks += now - mFrameStart;
    mCounts.frames++;
    if (now - mWindowStart >= WINDOW_TICKS) {
        LogWindow(now);
    }
}

void GfxRenderingAPIXsdk::FinishRender() {
    Timed timed(mCounts);
}

int GfxRenderingAPIXsdk::CreateFramebuffer() {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
    // 0 is the screen's (interpreter.cpp:4178).
    return ++mLastFramebuffer;
}

void GfxRenderingAPIXsdk::UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                                      bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                                      bool can_extract_depth) {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
}

void GfxRenderingAPIXsdk::StartDrawToFramebuffer(int fbId, float noiseScale) {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
}

void GfxRenderingAPIXsdk::CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1,
                                          int dstX0, int dstY0, int dstX1, int dstY1) {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
    LogFirstCall(mLoggedCopyFramebuffer, "CopyFramebuffer");
}

void GfxRenderingAPIXsdk::ClearFramebuffer(bool color, bool depth) {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
}

void GfxRenderingAPIXsdk::ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
    LogFirstCall(mLoggedReadFramebuffer, "ReadFramebufferToCPU");
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
    LogFirstCall(mLoggedPixelDepth, "GetPixelDepth");
    return {};
}

void* GfxRenderingAPIXsdk::GetFramebufferTextureId(int fbId) {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
    return nullptr;
}

void GfxRenderingAPIXsdk::SelectTextureFb(int fbId) {
    Timed timed(mCounts);
    mCounts.framebufferCalls++;
}

void GfxRenderingAPIXsdk::DeleteTexture(uint32_t texId) {
    Timed timed(mCounts);
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
    return reinterpret_cast<ImTextureID>(id);
}
} // namespace Fast
