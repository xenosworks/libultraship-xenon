#pragma once

#include "gfx_window_manager_api.h"

#include <cstdint>

namespace Fast {
class GfxRenderingAPIXsdk;

// The Xbox 360's screen through xsdk: always full screen at 1280x720, no mouse or keyboard.
class GfxWindowBackendXsdk final : public GfxWindowBackend {
  public:
    ~GfxWindowBackendXsdk() override = default;
    void Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width, uint32_t height,
              int32_t posX, int32_t posY) override;
    void Close() override;
    void SetKeyboardCallbacks(bool (*onKeyDown)(int scancode), bool (*onKeyUp)(int scancode),
                              void (*onAllKeysUp)()) override;
    void SetMouseCallbacks(bool (*onMouseButtonDown)(int btn), bool (*onMouseButtonUp)(int btn)) override;
    void SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool is_now_fullscreen)) override;
    void SetFullscreen(bool fullscreen) override;
    void GetActiveWindowRefreshRate(uint32_t* refreshRate) override;
    void SetCursorVisability(bool visability) override;
    void SetMousePos(int32_t posX, int32_t posY) override;
    void GetMousePos(int32_t* x, int32_t* y) override;
    void GetMouseDelta(int32_t* x, int32_t* y) override;
    void GetMouseWheel(float* x, float* y) override;
    bool GetMouseState(uint32_t btn) override;
    void SetMouseCapture(bool capture) override;
    bool IsMouseCaptured() override;
    void GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) override;
    void HandleEvents() override;
    bool IsFrameReady() override;
    void SwapBuffersBegin() override;
    void SwapBuffersEnd() override;
    double GetTime() override;
    void SetTargetFPS(int fps) override;
    void SetMaxFrameLatency(int latency) override;
    const char* GetKeyName(int scancode) override;
    bool CanDisableVsync() override;
    bool IsRunning() override;
    void Destroy() override;
    bool IsFullscreen() override;

    // The backend drawing into the screen, whose GPU memory goes before the GPU closes.
    void SetRenderer(GfxRenderingAPIXsdk* renderer);
    // Whether the last SwapBuffersBegin resolved the screen into a front buffer, clearing its eDRAM.
    bool Presented() const;

  private:
    GfxRenderingAPIXsdk* mRenderer = nullptr;
    bool mPresented = false;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    bool mGpuOpen = false;
    bool mPresenting = false;
    uint32_t mFrames = 0;
    // When the next frame may start, in nanoseconds of the monotonic clock.
    int64_t mFrameDueNs = 0;
};
} // namespace Fast
