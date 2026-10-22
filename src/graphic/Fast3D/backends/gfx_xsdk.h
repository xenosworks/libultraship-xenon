#pragma once

#include "gfx_rendering_api.h"

#include <cstdint>
#include <map>
#include <utility>

namespace Fast {
// What the interpreter asks of a shader (interpreter.cpp:1613).
struct ShaderProgram {
    uint8_t numInputs;
    bool usedTextures[2];
};

// Fast3D on xsdk's GPU layer. For now it draws nothing: it counts and times what Fast3D asks of a backend and logs it
// every 10 s (xsdk:docs/scope/61-fast3d.md, "What it takes", 1).
class GfxRenderingAPIXsdk final : public GfxRenderingAPI {
  public:
    ~GfxRenderingAPIXsdk() override = default;
    const char* GetName() override;
    int GetMaxTextureSize() override;
    GfxClipParameters GetClipParameters() override;
    void UnloadShader(ShaderProgram* oldPrg) override;
    void LoadShader(ShaderProgram* newPrg) override;
    ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint32_t shaderId1) override;
    ShaderProgram* LookupShader(uint64_t shaderId0, uint32_t shaderId1) override;
    void ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) override;
    uint32_t NewTexture() override;
    void SelectTexture(int tile, uint32_t textureId) override;
    void UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) override;
    void SetSamplerParameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) override;
    void SetDepthTestAndMask(bool depth_test, bool z_upd) override;
    void SetZmodeDecal(bool decal) override;
    void SetViewport(int x, int y, int width, int height) override;
    void SetScissor(int x, int y, int width, int height) override;
    void SetUseAlpha(bool useAlpha) override;
    void DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) override;
    void Init() override;
    void OnResize() override;
    void StartFrame() override;
    void EndFrame() override;
    void FinishRender() override;
    int CreateFramebuffer() override;
    void UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                     bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                     bool can_extract_depth) override;
    void StartDrawToFramebuffer(int fbId, float noiseScale) override;
    void CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0,
                         int dstX1, int dstY1) override;
    void ClearFramebuffer(bool color, bool depth) override;
    void ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) override;
    void ResolveMSAAColorBuffer(int fbIdTarger, int fbIdSrc) override;
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) override;
    void* GetFramebufferTextureId(int fbId) override;
    void SelectTextureFb(int fbId) override;
    void DeleteTexture(uint32_t texId) override;
    void SetTextureFilter(FilteringMode mode) override;
    FilteringMode GetTextureFilter() override;
    void SetSrgbMode() override;
    ImTextureID GetTextureById(int id) override;

  private:
    // A 10 s window's counts; ticks are the CPU's time base.
    struct Counts {
        uint32_t frames;
        uint32_t draws;
        uint64_t triangles;
        uint64_t floats;
        uint32_t shadersCreated;
        uint32_t shadersLoaded;
        uint32_t texturesCreated;
        uint32_t uploads;
        uint64_t uploadBytes;
        uint32_t framebufferCalls;
        uint64_t backendTicks;
        uint64_t frameTicks;
    };
    // Adds the CPU time from its making to its end to the window's.
    struct Timed {
        explicit Timed(Counts& counts);
        ~Timed();
        Counts& counts;
        uint32_t start;
    };

    void LogWindow(uint32_t now);
    void LogFirstCall(bool& logged, const char* name);

    std::map<std::pair<uint64_t, uint32_t>, ShaderProgram> mShaders;
    Counts mCounts = {};
    uint32_t mWindowStart = 0;
    uint32_t mFrameStart = 0;
    uint32_t mLastTextureId = 0;
    int mLastFramebuffer = 0;
    FilteringMode mFilter = FILTER_THREE_POINT;
    bool mLoggedPixelDepth = false;
    bool mLoggedReadFramebuffer = false;
    bool mLoggedCopyFramebuffer = false;
};
} // namespace Fast
