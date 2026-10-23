#include "gfx_xsdk_window.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "Context.h"
#include "gfx_xsdk.h"
#include "imgui_impl_xsdk.h"
#include "public/bridge/consolevariablebridge.h"
#include "window/Window.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/splash.h"
#include "xsdk/texture.h"

namespace Fast {
static constexpr uint32_t BUFFERS = 2;
static constexpr uint32_t SWAP_INTERVAL = 1;
static constexpr uint32_t REFRESH_RATE = 60;
// The Guide's first opening held a frame up 2.7 s (xsdk:docs/HARDWARE.md, "The Guide, milestone 37").
static constexpr uint32_t BEGIN_TIMEOUT_MS = 5000;

// The monotonic clock is the CPU's time base (xsdk:src/time.c:63-71).
static int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The frames game:\capture.txt names, whitespace apart, ascending; none without the file.
static std::vector<uint32_t> ReadCaptures() {
    std::vector<uint32_t> frames;
    FILE* file = std::fopen("game:\\capture.txt", "r");
    if (file == nullptr) {
        return frames;
    }
    unsigned frame;
    while (std::fscanf(file, "%u", &frame) == 1) {
        frames.push_back(frame);
    }
    std::fclose(file);
    std::sort(frames.begin(), frames.end());
    xsdk_log_write("gfx_xsdk: capturing %u frames named in game:\\capture.txt", (unsigned)frames.size());
    return frames;
}

// The frame resolved into a front buffer, which holds what B8G8R8A8 textures are resolved as (xsdk:src/gpu.c:617-620),
// read back once the GPU is done and written as a PPM.
static void Capture(uint32_t address, uint32_t width, uint32_t height, uint32_t frame) {
    const NTSTATUS waited = xsdk_gpu_wait(xsdk_gpu_fence(), 1000);
    XsdkTexture front;
    if (!NT_SUCCESS(waited) || !NT_SUCCESS(xsdk_texture_init(&front, XSDK_FORMAT_B8G8R8A8, width, height, 1, true))) {
        xsdk_log_write("gfx_xsdk: frame %u not captured", (unsigned)frame);
        return;
    }
    xsdk_texture_attach(&front, xsdk_present_pixels(address));
    std::vector<uint8_t> bgra(width * height * 4);
    xsdk_texture_download_rows(&front, 0, 0, height, bgra.data(), width * 4);
    char path[64];
    std::snprintf(path, sizeof(path), "game:\\capture_%u.ppm", (unsigned)frame);
    FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        xsdk_log_write("gfx_xsdk: %s not written", path);
        return;
    }
    std::fprintf(file, "P6\n%u %u\n255\n", (unsigned)width, (unsigned)height);
    std::vector<uint8_t> rgb(width * 3);
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t* texel = &bgra[(y * width + x) * 4];
            rgb[x * 3] = texel[2];
            rgb[x * 3 + 1] = texel[1];
            rgb[x * 3 + 2] = texel[0];
        }
        std::fwrite(rgb.data(), 1, rgb.size(), file);
    }
    std::fclose(file);
    xsdk_log_write("gfx_xsdk: frame %u captured to %s", (unsigned)frame, path);
}

void GfxWindowBackendXsdk::Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width,
                                uint32_t height, int32_t posX, int32_t posY) {
    mFullScreen = true;
    mWidth = 1280;
    mHeight = 720;
    NTSTATUS status = xsdk_gpu_open();
    if (!NT_SUCCESS(status)) {
        xsdk_log_write("gfx_xsdk: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
    } else {
        mGpuOpen = true;
        // The logo stays up while the game starts, until the first frame (xsdk:include/xsdk/splash.h).
        status = xsdk_splash_begin();
        if (!NT_SUCCESS(status)) {
            xsdk_log_write("gfx_xsdk: xsdk_splash_begin failed: 0x%08X", (unsigned)status);
        }
        status = xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &mWidth, &mHeight);
        if (!NT_SUCCESS(status)) {
            xsdk_log_write("gfx_xsdk: xsdk_present_open failed: 0x%08X", (unsigned)status);
            xsdk_splash_end();
        }
        mPresenting = NT_SUCCESS(status);
    }
    mFrameDueNs = NowNs();
    mCaptures = ReadCaptures();
    // The console's only input is its controllers: Back opens the menu (Gui.cpp, TOGGLE_PAD_BTN) and they move
    // through it, unless the player turned that off.
    CVarRegisterInteger(CVAR_IMGUI_CONTROLLER_NAV, 1);
    Ship::Context::GetInstance()->GetWindow()->GetGui()->Init(Ship::GuiWindowInitData{});
}

void GfxWindowBackendXsdk::Close() {
    mIsRunning = false;
}

void GfxWindowBackendXsdk::SetKeyboardCallbacks(bool (*onKeyDown)(int scancode), bool (*onKeyUp)(int scancode),
                                                void (*onAllKeysUp)()) {
    mOnKeyDown = onKeyDown;
    mOnKeyUp = onKeyUp;
}

void GfxWindowBackendXsdk::SetMouseCallbacks(bool (*onMouseButtonDown)(int btn), bool (*onMouseButtonUp)(int btn)) {
    mOnMouseButtonDown = onMouseButtonDown;
    mOnMouseButtonUp = onMouseButtonUp;
}

void GfxWindowBackendXsdk::SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool is_now_fullscreen)) {
    mOnFullscreenChanged = onFullscreenChanged;
}

void GfxWindowBackendXsdk::SetFullscreen(bool fullscreen) {
}

void GfxWindowBackendXsdk::GetActiveWindowRefreshRate(uint32_t* refreshRate) {
    *refreshRate = REFRESH_RATE;
}

void GfxWindowBackendXsdk::SetCursorVisability(bool visability) {
}

void GfxWindowBackendXsdk::SetMousePos(int32_t posX, int32_t posY) {
}

void GfxWindowBackendXsdk::GetMousePos(int32_t* x, int32_t* y) {
    *x = 0;
    *y = 0;
}

void GfxWindowBackendXsdk::GetMouseDelta(int32_t* x, int32_t* y) {
    *x = 0;
    *y = 0;
}

void GfxWindowBackendXsdk::GetMouseWheel(float* x, float* y) {
    *x = 0.0f;
    *y = 0.0f;
}

bool GfxWindowBackendXsdk::GetMouseState(uint32_t btn) {
    return false;
}

void GfxWindowBackendXsdk::SetMouseCapture(bool capture) {
}

bool GfxWindowBackendXsdk::IsMouseCaptured() {
    return false;
}

void GfxWindowBackendXsdk::GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
    *width = mWidth;
    *height = mHeight;
    *posX = 0;
    *posY = 0;
}

void GfxWindowBackendXsdk::HandleEvents() {
    // As the SDL2 backend does, leaving the controllers' coming and going to SDLAddRemoveDeviceEventHandler.
    SDL_Event event;
    SDL_PumpEvents();
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_CONTROLLERDEVICEADDED - 1) > 0 ||
           SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_CONTROLLERDEVICEREMOVED + 1, SDL_LASTEVENT) > 0) {
        if (event.type == SDL_QUIT) {
            Close();
        }
    }
}

// At the refresh rate or above, xsdk_present_begin paces each frame to vblank, as it waits for a buffer to come off
// screen; below it, frames are held here as well.
bool GfxWindowBackendXsdk::IsFrameReady() {
    const int64_t now = NowNs();
    if (mTargetFps <= 0 || (uint32_t)mTargetFps >= REFRESH_RATE) {
        mFrameDueNs = now;
        return true;
    }
    const int64_t periodNs = 1000000000LL / mTargetFps;
    if (mFrameDueNs > now) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(mFrameDueNs - now));
    } else if (now - mFrameDueNs > periodNs) {
        // Too far behind to catch up: the frames from here are counted from now.
        mFrameDueNs = now;
    }
    mFrameDueNs += periodNs;
    return true;
}

void GfxWindowBackendXsdk::SwapBuffersBegin() {
    mPresented = false;
    if (!mPresenting) {
        return;
    }
    uint32_t address;
    const NTSTATUS status = xsdk_present_begin(&address, BEGIN_TIMEOUT_MS);
    if (!NT_SUCCESS(status)) {
        xsdk_log_write("gfx_xsdk: xsdk_present_begin failed: 0x%08X", (unsigned)status);
        return;
    }
#ifdef XSDK_FAST3D_NULL
    // Nothing draws: each frame clears to a blue a step brighter, so a title that stops shows a still screen.
    const uint32_t clear = 0xFF000000 | (mFrames++ & 0x7F) << 1;
#else
    // The screen gfx_xsdk drew, framebuffer 0, cleared for the next frame as GL clears (gfx_opengl.cpp:800).
    const uint32_t clear = XSDK_CLEAR_ARGB;
#endif
    xsdk_gpu_resolve(address, mWidth, mHeight, clear);
    xsdk_present_end();
    mPresented = true;
    mPresents++;
    if (!mCaptures.empty() && mCaptures.front() == mPresents) {
        mCaptures.erase(mCaptures.begin());
        Capture(address, mWidth, mHeight, mPresents);
    }
}

void GfxWindowBackendXsdk::SwapBuffersEnd() {
}

double GfxWindowBackendXsdk::GetTime() {
    return NowNs() / 1e9;
}

void GfxWindowBackendXsdk::SetTargetFPS(int fps) {
    mTargetFps = fps;
}

void GfxWindowBackendXsdk::SetMaxFrameLatency(int latency) {
}

const char* GfxWindowBackendXsdk::GetKeyName(int scancode) {
    return "";
}

bool GfxWindowBackendXsdk::CanDisableVsync() {
    return false;
}

bool GfxWindowBackendXsdk::IsRunning() {
    return mIsRunning;
}

void GfxWindowBackendXsdk::Destroy() {
    // What draws through the GPU goes before it closes: the backend's memory, then ImGui's, whose backend Gui::Init
    // started from Init above.
    if (mRenderer != nullptr) {
        mRenderer->Release();
    }
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().BackendRendererUserData != nullptr) {
        ImGui_ImplXsdk_Shutdown();
    }
    if (mPresenting) {
        xsdk_present_close();
        mPresenting = false;
    }
    if (mGpuOpen) {
        const NTSTATUS status = xsdk_gpu_close();
        if (!NT_SUCCESS(status)) {
            xsdk_log_write("gfx_xsdk: xsdk_gpu_close failed: 0x%08X", (unsigned)status);
        }
        mGpuOpen = false;
    }
}

bool GfxWindowBackendXsdk::IsFullscreen() {
    return true;
}

void GfxWindowBackendXsdk::SetRenderer(GfxRenderingAPIXsdk* renderer) {
    mRenderer = renderer;
}

bool GfxWindowBackendXsdk::Presented() const {
    return mPresented;
}
} // namespace Fast
