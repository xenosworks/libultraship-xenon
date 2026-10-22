#include "gfx_xsdk_window.h"

#include <SDL2/SDL.h>
#include <chrono>
#include <thread>

#include "Context.h"
#include "gfx_xsdk.h"
#include "window/Window.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/splash.h"

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

bool GfxWindowBackendXsdk::IsFrameReady() {
    const int64_t periodNs = 1000000000LL / (mTargetFps > 0 ? mTargetFps : REFRESH_RATE);
    const int64_t now = NowNs();
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
    // The backend's GPU memory goes before the GPU closes.
    if (mRenderer != nullptr) {
        mRenderer->Release();
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
