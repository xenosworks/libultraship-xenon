#pragma once

#include "gfx_rendering_api.h"

#include <bitset>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "xsdk/gpu.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

struct CCFeatures;

namespace Fast {
class GfxWindowBackendXsdk;

// Float constants a shader reads its literals from, set together.
struct XsdkConstantRun {
    uint32_t first;
    uint32_t count;
    std::vector<float> values;
};

// What the interpreter asks of a shader (interpreter.cpp:1613), and what the backend draws it with.
struct ShaderProgram {
    uint8_t numInputs;
    bool usedTextures[2];
    // False where it couldn't be compiled or loaded: its draws are skipped.
    bool drawable;
    uint32_t vertexFloats;
    // Fast3D's textures 0 to 5: the fetch constant the pixel shader samples each from, or -1.
    int8_t fetchSlots[6];
    // The pixel shader's uniform block's first float constant, or -1.
    int32_t block;
    XsdkShader vertex;
    XsdkShader pixel;
    // The shaders point into their containers, which must outlive them.
    std::vector<uint32_t> vertexContainer;
    std::vector<uint32_t> pixelContainer;
    std::vector<XsdkConstantRun> vertexLiterals;
    std::vector<XsdkConstantRun> pixelLiterals;
};

// The window's clear colour, opaque black as gfx_opengl clears (gfx_opengl.cpp:800), 0xAARRGGBB.
constexpr uint32_t XSDK_CLEAR_ARGB = 0xFF000000;

// Fast3D on xsdk's GPU layer (xsdk:docs/scope/61-fast3d.md, "The design"). It counts and times every call and logs
// them every 10 s; built with XSDK_FAST3D_NULL it draws nothing, as step 1 measured the CPU alone.
class GfxRenderingAPIXsdk final : public GfxRenderingAPI {
  public:
    explicit GfxRenderingAPIXsdk(GfxWindowBackendXsdk* window);
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

    // Waits for the GPU and frees what the backend holds in GPU memory; the window calls it before closing the GPU.
    void Release();

  private:
    // A 10 s window's counts; ticks are the CPU's time base.
    struct Counts {
        uint32_t frames;
        uint32_t draws;
        uint32_t skippedDraws;
        uint64_t triangles;
        uint64_t floats;
        uint32_t shadersCreated;
        uint32_t shadersCompiled;
        uint32_t shadersRead;
        uint32_t shadersLoaded;
        uint32_t texturesCreated;
        uint32_t uploads;
        uint64_t uploadBytes;
        uint32_t framebufferCalls;
        uint64_t backendTicks;
        uint64_t copyTicks;
        // DrawTriangles' own, its copy included, and UploadTexture's, each part of backendTicks.
        uint64_t drawTicks;
        uint64_t uploadTicks;
        uint64_t frameTicks;
        // The GPU's clock, 498.75 a microsecond, from when the command processor reached each timed frame to when the
        // GPU went idle after it.
        uint64_t gpuTicks;
        uint32_t gpuFrames;
    };
    // Adds the CPU time from its making to its end to the window's, and to part's if one is named.
    struct Timed {
        explicit Timed(Counts& counts, uint64_t* part = nullptr);
        ~Timed();
        Counts& counts;
        uint64_t* part;
        uint32_t start;
    };
    // Where a texture's texels sit: 4 KB pages of a shared block, or an allocation of its own.
    struct Memory {
        volatile uint32_t* texels;
        int32_t block;
        uint32_t firstPage;
        uint32_t pages;
    };
    struct Texture {
        // Its address is 0 until it's uploaded; ImGui takes a pointer to it as an ImTextureID.
        XsdkTexture texture;
        XsdkSamplerState sampler;
        Memory memory;
        // Raised whenever its fetch constant changes, so draws set it again.
        uint32_t generation;
        bool threePoint;
    };
    struct Framebuffer {
        uint32_t width;
        uint32_t height;
        // Its colour's texture, which the eDRAM target is resolved into; none for the screen, framebuffer 0.
        uint32_t texture;
        // Whether its colour and depth fit in eDRAM; draws to one that doesn't are skipped.
        bool fits;
        // Drawn since its target was set.
        bool drawn;
    };
    struct PageBlock {
        volatile uint32_t* memory;
        std::bitset<256> used;
    };
    // GL's lower-left rectangle (interpreter.cpp, AdjustVIewportOrScissor).
    struct Rectangle {
        int x;
        int y;
        int width;
        int height;
    };
    struct FetchBinding {
        const Texture* texture;
        uint32_t generation;
    };
    struct PendingFree {
        Memory memory;
        // The frame fence after which it goes; 0 until this frame's is queued.
        uint32_t fence;
    };
    struct ClockSlot {
        uint32_t fence;
        bool pending;
    };

    void LogWindow(uint32_t now);
    void LogOnce(bool& logged, const char* what);
    Memory Allocate(uint32_t bytes);
    void FreeAfterFrame(Memory& memory);
    void FreeNow(Memory& memory);
    void ReleasePassed();
    bool ReplaceTexels(Texture& texture, uint32_t width, uint32_t height, bool framebuffer);
    void SetSampler(Texture& texture, bool linear_filter, uint32_t cms, uint32_t cmt);
    const Texture& TextureFor(uint32_t id) const;
    void LoadProgram(ShaderProgram& prg, const ::CCFeatures& features, uint64_t id0, uint32_t id1);
    bool FinishProgram(ShaderProgram& prg, const std::string& name, const std::string& vertexGlsl,
                       const std::string& fragmentGlsl, const XsdkVertexElement* elements, uint32_t elementCount);
    void SetTarget();
    void LeaveFramebuffer();
    void Resolved();
    void ApplyState();
    void ApplyProgram(const ShaderProgram& prg);
    void ApplyTextures(const ShaderProgram& prg);
    void ApplyBlock(const ShaderProgram& prg);
    uint32_t WriteVertices(const float* floats, uint32_t bytes);
    void NextSegment();
    bool WaitFence(uint32_t fence, const char* what);
    void ReadClocks();

    GfxWindowBackendXsdk* mWindow;
    std::map<std::pair<uint64_t, uint32_t>, ShaderProgram> mShaders;
    // Stable as they grow, as ImGui holds pointers to their XsdkTextures; texture 0 is opaque black, sampled where a
    // texture has no texels.
    std::deque<Texture> mTextures;
    std::vector<Framebuffer> mFramebuffers;
    std::vector<PageBlock> mBlocks;
    std::vector<PendingFree> mPendingFrees;
    ShaderProgram mClearProgram = {};
    ShaderProgram* mShader = nullptr;
    const ShaderProgram* mAppliedShader = nullptr;
    std::string mShaderFolder;
    Counts mCounts = {};
    uint32_t mWindowStart = 0;
    uint32_t mFrameStart = 0;
    uint32_t mFrameCount = 0;
    int mLastFramebuffer = 0;
    FilteringMode mFilter = FILTER_THREE_POINT;

    // State as Fast3D last set it, and what draws must set again.
    uint32_t mCurrentTextureIds[6] = {};
    int mCurrentTile = 0;
    bool mUseAlpha = false;
    Rectangle mViewport = {};
    Rectangle mScissor = {};
    float mNoiseScale = 1.0f;
    uint32_t mDirty = 0;
    FetchBinding mBound[32] = {};
    float mBlock[12] = {};
    int32_t mBlockRegister = -1;
    bool mBlockValid = false;

    // The target: the framebuffer drawn to, whether a resolve undid it, and the framebuffer whose eDRAM a resolve
    // left cleared, or -1.
    int mCurrentFb = 0;
    bool mTargetUndone = true;
    int mClearedFb = -1;
    uint32_t mSystemDraws = 0;

    // The states draws pick from: depth by test, mask and decal; blending off, on, and writing no colour.
    XsdkDepthStencilState mDepthStates[8] = {};
    XsdkBlendState mBlendOpaque = {};
    XsdkBlendState mBlendAlpha = {};
    XsdkBlendState mBlendNoColor = {};
    XsdkRasterState mRasterPlain = {};

    // The vertex ring, its segments each reused once the fence queued as it was left has passed, then the clock's
    // words.
    volatile uint32_t* mRing = nullptr;
    uint32_t mRingAddress = 0;
    uint32_t mRingSegment = 0;
    uint32_t mRingOffset = 0;
    uint32_t mRingFences[8] = {};
    ClockSlot mClocks[4] = {};
    bool mTimingFrame = false;

    bool mLoggedPixelDepth = false;
    bool mLoggedReadFramebuffer = false;
    bool mLoggedCopyFramebuffer = false;
    bool mLoggedMsaa = false;
    bool mLoggedScreenLost = false;
    bool mLoggedTooBig = false;
    bool mLoggedLayout = false;
};
} // namespace Fast
