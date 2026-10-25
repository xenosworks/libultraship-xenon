// fast3d_check: gfx_xsdk's checks on the Xbox 360 (xsdk:docs/scope/61-fast3d.md, "Checks"). Each check calls the
// backend as Fast3D's interpreter does (gfx_rendering_api.h; interpreter.cpp, the vertex it writes at 1617-1762) to
// draw into a framebuffer of its own, resolves that and reads its texels back. Every expected texel follows from the
// check's inputs: colours are 0 or 255 a channel unless a blend halves them, and the pixels compared sit inside each
// region. It logs "PASS: <check>" or "FAIL: <check>", then "tests: <passed> passed, <failed> failed"; breaks/ breaks it
// on purpose.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include "Context.h"
#include "graphic/Fast3D/backends/gfx_xsdk.h"
#include "graphic/Fast3D/interpreter.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"
#include "xsdk/texture.h"

namespace {
using Fast::GfxRenderingAPIXsdk;
using Fast::ShaderProgram;

constexpr uint32_t SPLASH_MS = 2000;
constexpr uint32_t FENCE_TIMEOUT_MS = 1000;
// The framebuffer each check draws into, and the second the game-framebuffer check samples.
constexpr uint32_t SIZE = 64;
constexpr uint32_t SOURCE_SIZE = 32;
constexpr int MAX_LOGGED = 4;
// A channel's leeway where a blend halves it, as the GPU may round half either way.
constexpr int BLEND_TOLERANCE = 2;

// 0xRRGGBB.
constexpr uint32_t BLACK = 0x000000;
constexpr uint32_t RED = 0xFF0000;
constexpr uint32_t GREEN = 0x00FF00;
constexpr uint32_t BLUE = 0x0000FF;
constexpr uint32_t YELLOW = 0xFFFF00;
constexpr uint32_t MAGENTA = 0xFF00FF;
constexpr uint32_t CYAN = 0x00FFFF;
constexpr uint32_t WHITE = 0xFFFFFF;

int sPassed;
int sFailed;
int sScreen;
int sTarget;
int sSource;

void Report(const char* name, bool passed) {
    xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
    (passed ? sPassed : sFailed)++;
}

// A cycle's four items a, b, c and d, (a - b) * c + d, for its colour or its alpha.
uint64_t Cycle(int a, int b, int c, int d) {
    return (uint64_t)a | (uint64_t)b << 4 | (uint64_t)c << 8 | (uint64_t)d << 12;
}

// shader_id0: each cycle's colour then alpha, 16 bits apart (gfx_cc_get_features).
uint64_t Combiner(uint64_t color0, uint64_t alpha0 = 0, uint64_t color1 = 0, uint64_t alpha1 = 0) {
    return color0 | alpha0 << 16 | color1 << 32 | alpha1 << 48;
}

std::vector<float> Rgb(uint32_t rgb) {
    return { (float)(rgb >> 16 & 0xFF) / 255.0f, (float)(rgb >> 8 & 0xFF) / 255.0f, (float)(rgb & 0xFF) / 255.0f };
}

std::vector<float> Rgba(uint32_t rgb, float alpha) {
    std::vector<float> out = Rgb(rgb);
    out.push_back(alpha);
    return out;
}

std::vector<float> Join(std::initializer_list<std::vector<float>> parts) {
    std::vector<float> out;
    for (const std::vector<float>& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

// The program for a combiner, made current as the interpreter makes it (interpreter.cpp,
// LookupOrCreateShaderProgram).
ShaderProgram* Program(GfxRenderingAPIXsdk& backend, uint64_t id0, uint32_t id1) {
    ShaderProgram* program = backend.LookupShader(id0, id1);
    if (program == nullptr) {
        return backend.CreateAndLoadNewShader(id0, id1);
    }
    backend.LoadShader(program);
    return program;
}

// Two triangles over clip space's x0 to x1 and y0 to y1, y 1 the first row, at depth z and w 1. attributes gives each
// corner's floats after its position, from s 0 at x0 to 1 at x1 and t 0 at y0 to 1 at y1.
void Rect(GfxRenderingAPIXsdk& backend, float x0, float y0, float x1, float y1, float z,
          const std::function<std::vector<float>(float s, float t)>& attributes) {
    static const float corners[6][2] = { { 0, 0 }, { 0, 1 }, { 1, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    std::vector<float> floats;
    for (const auto& corner : corners) {
        const float s = corner[0], t = corner[1];
        const float position[4] = { x0 + (x1 - x0) * s, y0 + (y1 - y0) * t, z, 1.0f };
        floats.insert(floats.end(), position, position + 4);
        const std::vector<float> rest = attributes(s, t);
        floats.insert(floats.end(), rest.begin(), rest.end());
    }
    backend.DrawTriangles(floats.data(), floats.size(), 2);
}

// The left or right half of the target, or all of it.
void Left(GfxRenderingAPIXsdk& backend, float z, const std::function<std::vector<float>(float, float)>& attributes) {
    Rect(backend, -1.0f, 1.0f, 0.0f, -1.0f, z, attributes);
}

void Right(GfxRenderingAPIXsdk& backend, float z, const std::function<std::vector<float>(float, float)>& attributes) {
    Rect(backend, 0.0f, 1.0f, 1.0f, -1.0f, z, attributes);
}

void Whole(GfxRenderingAPIXsdk& backend, float z, const std::function<std::vector<float>(float, float)>& attributes) {
    Rect(backend, -1.0f, 1.0f, 1.0f, -1.0f, z, attributes);
}

// A texture of 0xRRGGBB texels, opaque, rows first, in tile's slot, sampled at points with the wrap modes given.
uint32_t Texture(GfxRenderingAPIXsdk& backend, int tile, const std::vector<uint32_t>& texels, uint32_t width,
                 uint32_t height, uint32_t cms = G_TX_WRAP, uint32_t cmt = G_TX_WRAP) {
    std::vector<uint8_t> rgba;
    for (uint32_t texel : texels) {
        rgba.insert(rgba.end(), { (uint8_t)(texel >> 16), (uint8_t)(texel >> 8), (uint8_t)texel, 0xFF });
    }
    const uint32_t id = backend.NewTexture();
    backend.SelectTexture(tile, id);
    backend.UploadTexture(rgba.data(), width, height);
    backend.SetSamplerParameters(tile, false, cms, cmt);
    return id;
}

// A frame drawing into framebuffer fb, cleared, its viewport and scissor the whole of it, depth and blending off.
void Into(GfxRenderingAPIXsdk& backend, int fb, uint32_t size) {
    backend.StartDrawToFramebuffer(fb, 1.0f);
    backend.ClearFramebuffer(true, true);
    backend.SetViewport(0, 0, (int)size, (int)size);
    backend.SetScissor(0, 0, (int)size, (int)size);
    backend.SetDepthTestAndMask(false, false);
    backend.SetZmodeDecal(false);
    backend.SetUseAlpha(false);
}

// Ends the frame, which resolves the target into its texture as Fast3D leaves it, and reads that back as 0xRRGGBB.
std::vector<uint32_t> Finish(GfxRenderingAPIXsdk& backend) {
    backend.StartDrawToFramebuffer(sScreen, 1.0f);
    backend.EndFrame();
    backend.FinishRender();
    std::vector<uint32_t> pixels(SIZE * SIZE, 0);
    const NTSTATUS waited = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
    const XsdkTexture* texture = static_cast<const XsdkTexture*>(backend.GetFramebufferTextureId(sTarget));
    if (!NT_SUCCESS(waited) || texture == nullptr) {
        xsdk_log_write("fast3d_check: no framebuffer to read: wait 0x%08X", (unsigned)waited);
        return pixels;
    }
    std::vector<uint8_t> bytes(SIZE * SIZE * 4);
    xsdk_texture_download_rows(texture, 0, 0, SIZE, bytes.data(), SIZE * 4);
    for (uint32_t i = 0; i < SIZE * SIZE; i++) {
        pixels[i] = (uint32_t)bytes[i * 4] << 16 | (uint32_t)bytes[i * 4 + 1] << 8 | bytes[i * 4 + 2];
    }
    return pixels;
}

void Start(GfxRenderingAPIXsdk& backend) {
    backend.StartFrame();
    Into(backend, sTarget, SIZE);
}

// Whether every pixel inside x0 to x1 and y0 to y1, ends exclusive, is what expected gives, each channel within
// tolerance; logs the first few that aren't.
bool Matches(const std::vector<uint32_t>& pixels, const char* name, int x0, int y0, int x1, int y1,
             const std::function<uint32_t(int x, int y)>& expected, int tolerance = 0) {
    int wrong = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            const uint32_t got = pixels[y * SIZE + x];
            const uint32_t want = expected(x, y);
            bool close = true;
            for (int shift = 0; shift < 24; shift += 8) {
                close = close && std::abs((int)(got >> shift & 0xFF) - (int)(want >> shift & 0xFF)) <= tolerance;
            }
            if (!close && wrong++ < MAX_LOGGED) {
                xsdk_log_write("%s: (%d, %d) is %06X, not %06X", name, x, y, (unsigned)got, (unsigned)want);
            }
        }
    }
    return wrong == 0;
}

// The target's halves, a pixel apart from where they meet.
bool Halves(const std::vector<uint32_t>& pixels, const char* name, uint32_t left, uint32_t right, int tolerance = 0) {
    const bool l = Matches(pixels, name, 0, 0, SIZE / 2 - 1, SIZE, [&](int, int) { return left; }, tolerance);
    const bool r = Matches(pixels, name, SIZE / 2 + 1, 0, SIZE, SIZE, [&](int, int) { return right; }, tolerance);
    return l && r;
}

// A 2x2 texture drawn over the whole target with s and t from 0 to 1: the texel under pixel (x, y).
uint32_t Under(const uint32_t texels[4], int x, int y) {
    return texels[(y * 2 / (int)SIZE) * 2 + x * 2 / (int)SIZE];
}

// Its pixels a pixel away from the texels' edges, which fall between pixel centres.
bool Quadrants(const std::vector<uint32_t>& pixels, const char* name,
               const std::function<uint32_t(int, int)>& expected) {
    bool all = true;
    for (int qy = 0; qy < 2; qy++) {
        for (int qx = 0; qx < 2; qx++) {
            const int x0 = qx * SIZE / 2, y0 = qy * SIZE / 2;
            all = Matches(pixels, name, x0 + 1, y0 + 1, x0 + SIZE / 2 - 1, y0 + SIZE / 2 - 1, expected) && all;
        }
    }
    return all;
}

const uint32_t FOUR[4] = { WHITE, RED, GREEN, BLUE };
const uint32_t OTHER_FOUR[4] = { CYAN, MAGENTA, YELLOW, BLACK };

void CheckSingle(GfxRenderingAPIXsdk& backend) {
    Start(backend);
    Program(backend, Combiner(Cycle(SHADER_0, SHADER_0, SHADER_0, SHADER_INPUT_1)), 0);
    Whole(backend, 0.5f, [](float, float) { return Rgb(MAGENTA); });
    const std::vector<uint32_t> pixels = Finish(backend);
    Report("combiner_single", Matches(pixels, "combiner_single", 0, 0, SIZE, SIZE, [](int, int) { return MAGENTA; }));
}

void CheckMultiply(GfxRenderingAPIXsdk& backend) {
    Start(backend);
    Texture(backend, 0, { FOUR[0], FOUR[1], FOUR[2], FOUR[3] }, 2, 2);
    Program(backend, Combiner(Cycle(SHADER_TEXEL0, SHADER_0, SHADER_INPUT_1, SHADER_0)), 0);
    Whole(backend, 0.5f, [](float s, float t) { return Join({ { s, t }, Rgb(YELLOW) }); });
    const std::vector<uint32_t> pixels = Finish(backend);
    Report("combiner_multiply",
           Quadrants(pixels, "combiner_multiply", [](int x, int y) { return Under(FOUR, x, y) & YELLOW; }));
}

// Both texture slots: red and blue from texture 0, green from texture 1.
void CheckMixBothTextures(GfxRenderingAPIXsdk& backend) {
    Start(backend);
    Texture(backend, 0, { FOUR[0], FOUR[1], FOUR[2], FOUR[3] }, 2, 2);
    Texture(backend, 1, { OTHER_FOUR[0], OTHER_FOUR[1], OTHER_FOUR[2], OTHER_FOUR[3] }, 2, 2);
    Program(backend, Combiner(Cycle(SHADER_TEXEL0, SHADER_TEXEL1, SHADER_INPUT_1, SHADER_TEXEL1)), 0);
    Whole(backend, 0.5f, [](float s, float t) { return Join({ { s, t }, { s, t }, Rgb(MAGENTA) }); });
    const std::vector<uint32_t> pixels = Finish(backend);
    Report("combiner_mix_both_textures", Quadrants(pixels, "combiner_mix_both_textures", [](int x, int y) {
               return (Under(FOUR, x, y) & MAGENTA) | (Under(OTHER_FOUR, x, y) & GREEN);
           }));
}

// (1 - input 1) * texel 0 + input 2: input 1 takes texel 0's blue out and input 2 puts full blue in. No channel passes
// 1, where the combiner wraps as the N64's does (gfx_xsdk_glsl.cpp, WRAP).
void CheckGeneral(GfxRenderingAPIXsdk& backend) {
    Start(backend);
    Texture(backend, 0, { FOUR[0], FOUR[1], FOUR[2], FOUR[3] }, 2, 2);
    Program(backend, Combiner(Cycle(SHADER_1, SHADER_INPUT_1, SHADER_TEXEL0, SHADER_INPUT_2)), 0);
    Whole(backend, 0.5f, [](float s, float t) { return Join({ { s, t }, Rgb(BLUE), Rgb(BLUE) }); });
    const std::vector<uint32_t> pixels = Finish(backend);
    Report("combiner_general",
           Quadrants(pixels, "combiner_general", [](int x, int y) { return (Under(FOUR, x, y) & YELLOW) | BLUE; }));
}

// Texel 0 in the first cycle, then the combined colour times input 1 in the second.
void CheckTwoCycles(GfxRenderingAPIXsdk& backend) {
    Start(backend);
    Texture(backend, 0, { FOUR[0], FOUR[1], FOUR[2], FOUR[3] }, 2, 2);
    Texture(backend, 1, { BLACK, BLACK, BLACK, BLACK }, 2, 2);
    Program(backend,
            Combiner(Cycle(SHADER_0, SHADER_0, SHADER_0, SHADER_TEXEL0), 0,
                     Cycle(SHADER_COMBINED, SHADER_0, SHADER_INPUT_1, SHADER_0)),
            (uint32_t)SHADER_OPT(_2CYC));
    Whole(backend, 0.5f, [](float s, float t) { return Join({ { s, t }, { s, t }, Rgb(CYAN) }); });
    const std::vector<uint32_t> pixels = Finish(backend);
    Report("combiner_two_cycles",
           Quadrants(pixels, "combiner_two_cycles", [](int x, int y) { return Under(FOUR, x, y) & CYAN; }));
}

// White at half alpha over blue, blended as gfx_opengl blends (gfx_opengl.cpp:671).
void CheckAlpha(GfxRenderingAPIXsdk& backend) {
    Start(backend);
    const uint64_t single = Cycle(SHADER_0, SHADER_0, SHADER_0, SHADER_INPUT_1);
    Program(backend, Combiner(single, single), (uint32_t)SHADER_OPT(ALPHA));
    backend.SetUseAlpha(true);
    Whole(backend, 0.5f, [](float, float) { return Rgba(BLUE, 1.0f); });
    Whole(backend, 0.5f, [](float, float) { return Rgba(WHITE, 0.5f); });
    const std::vector<uint32_t> pixels = Finish(backend);
    Report("alpha_blend",
           Matches(pixels, "alpha_blend", 0, 0, SIZE, SIZE, [](int, int) { return 0x8080FFu; }, BLEND_TOLERANCE));
}

// Blue under fog: all fog on the left, none on the right.
void CheckFog(GfxRenderingAPIXsdk& backend) {
    Start(backend);
    Program(backend, Combiner(Cycle(SHADER_0, SHADER_0, SHADER_0, SHADER_INPUT_1)), (uint32_t)SHADER_OPT(FOG));
    Left(backend, 0.5f, [](float, float) { return Join({ Rgba(RED, 1.0f), Rgb(BLUE) }); });
    Right(backend, 0.5f, [](float, float) { return Join({ Rgba(RED, 0.0f), Rgb(BLUE) }); });
    Report("fog", Halves(Finish(backend), "fog", RED, BLUE));
}

// A 2x2 texture's first row, red then green, across s from -1 to 3 under each wrap mode (gfx_xsdk.cpp,
// AddressMode): 16 pixels a repeat, 8 a texel.
void CheckWrap(GfxRenderingAPIXsdk& backend, const char* name, uint32_t mode, int (*texel)(double s)) {
    Start(backend);
    Texture(backend, 0, { RED, GREEN, BLUE, WHITE }, 2, 2, mode, G_TX_WRAP);
    Program(backend, Combiner(Cycle(SHADER_0, SHADER_0, SHADER_0, SHADER_TEXEL0)), 0);
    Whole(backend, 0.5f, [](float s, float) { return std::vector<float>{ -1.0f + 4.0f * s, 0.25f }; });
    const std::vector<uint32_t> pixels = Finish(backend);
    bool all = true;
    for (int block = 0; block < (int)SIZE / 8; block++) {
        const double s = -1.0 + 4.0 * (block * 8 + 4) / SIZE;
        const uint32_t want = texel(s) == 0 ? RED : GREEN;
        all = Matches(pixels, name, block * 8 + 1, 0, block * 8 + 7, SIZE, [&](int, int) { return want; }) && all;
    }
    Report(name, all);
}

int TexelWrapped(double s) {
    return (int)std::floor((s - std::floor(s)) * 2.0);
}

int TexelMirrored(double s) {
    const double m = s - 2.0 * std::floor(s / 2.0);
    return (int)std::floor((m < 1.0 ? m : 2.0 - m) * 2.0);
}

int TexelClamped(double s) {
    return s < 0.5 ? 0 : 1;
}

int TexelMirroredOnce(double s) {
    return std::fabs(s) < 0.5 ? 0 : 1;
}

// The shader's clamp of s to half a texel (gfx_xsdk_glsl.cpp, o_clamp) reads the texture's size from the block: a
// 2x2 texture's on the left, then a 16x2 one's on the right, texel 0 red and the rest green, s from -1 to 0.5. A size
// left from the left half's draw clamps s at a quarter, to green.
void CheckTextureSize(GfxRenderingAPIXsdk& backend) {
    Start(backend);
    const uint32_t small = Texture(backend, 0, { GREEN, GREEN, GREEN, GREEN }, 2, 2);
    std::vector<uint32_t> wide(32, GREEN);
    wide[0] = RED;
    wide[16] = RED;
    const uint32_t large = Texture(backend, 0, wide, 16, 2);
    Program(backend, Combiner(Cycle(SHADER_0, SHADER_0, SHADER_0, SHADER_TEXEL0)),
            (uint32_t)SHADER_OPT(TEXEL0_CLAMP_S));
    backend.SelectTexture(0, small);
    Left(backend, 0.5f, [](float s, float t) { return std::vector<float>{ s, t, 0.75f }; });
    backend.SelectTexture(0, large);
    Right(backend, 0.5f,
          [](float s, float t) { return std::vector<float>{ -1.0f + 1.5f * s, t, 1.0f - 0.5f / 16.0f }; });
    const std::vector<uint32_t> pixels = Finish(backend);
    const bool left =
        Matches(pixels, "texture_size_constant", 0, 0, SIZE / 2 - 1, SIZE, [](int, int) { return GREEN; });
    const bool right = Matches(pixels, "texture_size_constant", SIZE / 2 + 1, 0, SIZE, SIZE, [](int x, int) {
        const double s = -1.0 + 1.5 * (x - (int)SIZE / 2 + 0.5) / (SIZE / 2);
        return std::max(s, 0.5 / 16.0) < 1.0 / 16.0 ? RED : GREEN;
    });
    Report("texture_size_constant", left && right);
}

// Two small textures share a block of pages (gfx_xsdk.cpp, Allocate): red drawn left, green right.
void CheckSharedBlock(GfxRenderingAPIXsdk& backend) {
    Start(backend);
    const uint32_t red = Texture(backend, 0, { RED, RED, RED, RED }, 2, 2);
    const uint32_t green = Texture(backend, 0, { GREEN, GREEN, GREEN, GREEN }, 2, 2);
    Program(backend, Combiner(Cycle(SHADER_0, SHADER_0, SHADER_0, SHADER_TEXEL0)), 0);
    backend.SelectTexture(0, red);
    Left(backend, 0.5f, [](float s, float t) { return std::vector<float>{ s, t }; });
    backend.SelectTexture(0, green);
    Right(backend, 0.5f, [](float s, float t) { return std::vector<float>{ s, t }; });
    Report("textures_share_a_block", Halves(Finish(backend), "textures_share_a_block", RED, GREEN));
}

// Blue written at depth 0.5, then at the same depth a decal red on the left and a plain draw green on the right: the
// decal passes (gfx_xsdk.cpp, ApplyState) and the plain draw doesn't.
void CheckDecal(GfxRenderingAPIXsdk& backend) {
    Start(backend);
    Program(backend, Combiner(Cycle(SHADER_0, SHADER_0, SHADER_0, SHADER_INPUT_1)), 0);
    backend.SetDepthTestAndMask(true, true);
    Whole(backend, 0.5f, [](float, float) { return Rgb(BLUE); });
    backend.SetDepthTestAndMask(true, false);
    backend.SetZmodeDecal(true);
    Left(backend, 0.5f, [](float, float) { return Rgb(RED); });
    backend.SetZmodeDecal(false);
    Right(backend, 0.5f, [](float, float) { return Rgb(GREEN); });
    Report("decal", Halves(Finish(backend), "decal", RED, BLUE));
}

// Past the far plane on the left and before the near one on the right, both drawn, as GL_DEPTH_CLAMP draws them
// (gfx_opengl.cpp:668; gfx_xsdk_glsl.cpp, the vertex template's clamp).
void CheckDepthClamp(GfxRenderingAPIXsdk& backend) {
    Start(backend);
    Program(backend, Combiner(Cycle(SHADER_0, SHADER_0, SHADER_0, SHADER_INPUT_1)), 0);
    Left(backend, 2.0f, [](float, float) { return Rgb(GREEN); });
    Right(backend, -1.0f, [](float, float) { return Rgb(GREEN); });
    Report("depth_clamp", Halves(Finish(backend), "depth_clamp", GREEN, GREEN));
}

// The game's framebuffer: red and green halves drawn into a second framebuffer, then sampled from its texture
// (SelectTextureFb) over the whole target.
void CheckGameFramebuffer(GfxRenderingAPIXsdk& backend) {
    backend.StartFrame();
    Into(backend, sSource, SOURCE_SIZE);
    Program(backend, Combiner(Cycle(SHADER_0, SHADER_0, SHADER_0, SHADER_INPUT_1)), 0);
    Left(backend, 0.5f, [](float, float) { return Rgb(RED); });
    Right(backend, 0.5f, [](float, float) { return Rgb(GREEN); });
    Into(backend, sTarget, SIZE);
    backend.SelectTextureFb(sSource);
    Program(backend, Combiner(Cycle(SHADER_0, SHADER_0, SHADER_0, SHADER_TEXEL0)), 0);
    Whole(backend, 0.5f, [](float s, float t) { return std::vector<float>{ s, t }; });
    // Its texture is filtered and wraps (CreateFramebuffer), so pixels near where the halves meet, and at the sides,
    // take some of the other half.
    const std::vector<uint32_t> pixels = Finish(backend);
    const bool left = Matches(pixels, "game_framebuffer", 2, 0, SIZE / 2 - 4, SIZE, [](int, int) { return RED; });
    const bool right =
        Matches(pixels, "game_framebuffer", SIZE / 2 + 4, 0, SIZE - 2, SIZE, [](int, int) { return GREEN; });
    Report("game_framebuffer", left && right);
}
} // namespace

int main() {
    xsdk_log_write("fast3d_check: gfx_xsdk's checks");
    const NTSTATUS splash = xsdk_splash_show(SPLASH_MS);
    if (!NT_SUCCESS(splash)) {
        xsdk_log_write("fast3d_check: xsdk_splash_show failed: 0x%08X", (unsigned)splash);
    }
    NTSTATUS status = xsdk_gpu_open();
    if (!NT_SUCCESS(status)) {
        xsdk_log_write("fast3d_check: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
        return 1;
    }
    // The decal's depth bias reads a console variable (gfx_xsdk.cpp, ApplyState). The context is never destroyed:
    // ~Context saves the window's settings first (Context.cpp:35), and this title has no window, so destroying it as
    // main returned crashed the console.
    std::shared_ptr<Ship::Context>& context = *new std::shared_ptr<Ship::Context>(
        Ship::Context::CreateUninitializedInstance("fast3d_check", "f3dc", "fast3d_check.cfg.json"));
    if (!context->InitConfiguration() || !context->InitConsoleVariables()) {
        xsdk_log_write("fast3d_check: libultraship's configuration couldn't be set up");
        return 1;
    }

    {
        GfxRenderingAPIXsdk backend(nullptr);
        backend.Init();
        sScreen = 0;
        backend.UpdateFramebufferParameters(sScreen, 1280, 720, 1, false, true, true, true);
        sTarget = backend.CreateFramebuffer();
        backend.UpdateFramebufferParameters(sTarget, SIZE, SIZE, 1, true, true, true, true);
        sSource = backend.CreateFramebuffer();
        backend.UpdateFramebufferParameters(sSource, SOURCE_SIZE, SOURCE_SIZE, 1, true, true, true, true);

        CheckSingle(backend);
        CheckMultiply(backend);
        CheckMixBothTextures(backend);
        CheckGeneral(backend);
        CheckTwoCycles(backend);
        CheckAlpha(backend);
        CheckFog(backend);
        CheckWrap(backend, "wrap", G_TX_WRAP, TexelWrapped);
        CheckWrap(backend, "wrap_mirror", G_TX_MIRROR | G_TX_WRAP, TexelMirrored);
        CheckWrap(backend, "wrap_clamp", G_TX_CLAMP, TexelClamped);
        CheckWrap(backend, "wrap_mirror_once", G_TX_MIRROR | G_TX_CLAMP, TexelMirroredOnce);
        CheckTextureSize(backend);
        CheckSharedBlock(backend);
        CheckDecal(backend);
        CheckDepthClamp(backend);
        CheckGameFramebuffer(backend);
        backend.Release();
    }

    status = xsdk_gpu_close();
    if (!NT_SUCCESS(status)) {
        xsdk_log_write("fast3d_check: xsdk_gpu_close failed: 0x%08X", (unsigned)status);
    }
    xsdk_log_write("tests: %d passed, %d failed", sPassed, sFailed);
    return sFailed != 0;
}
