#include "SDLAudioPlayer.h"
#include <spdlog/spdlog.h>

namespace Ship {

SDLAudioPlayer::~SDLAudioPlayer() {
    SPDLOG_TRACE("destruct SDL audio player");
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool SDLAudioPlayer::DoInit() {
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        SPDLOG_ERROR("SDL init error: %s\n", SDL_GetError());
        return false;
    }
    mNumChannels = this->GetAudioChannels() == AudioChannelsSetting::audioSurround51 ? 6 : 2;
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = this->GetSampleRate();
    want.format = AUDIO_S16SYS;
    want.channels = mNumChannels;
    want.samples = this->GetSampleLength();
    want.callback = NULL;
#ifdef __XSDK__
    // SDL's device thread on hardware thread 5, off the main thread's core (SDL-xenon:include/SDL3/SDL_hints.h).
    if (!SDL_SetHint("SDL_AUDIO_XSDK_HARDWARE_THREAD", "5")) {
        SPDLOG_WARN("SDL_SetHint(SDL_AUDIO_XSDK_HARDWARE_THREAD) failed");
    }
#endif
    mDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (mDevice == 0) {
        SPDLOG_ERROR("SDL_OpenAudio error: {}", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(mDevice, 0);
    return true;
}

int SDLAudioPlayer::Buffered() {
    return SDL_GetQueuedAudioSize(mDevice) / (sizeof(int16_t) * mNumChannels);
}

void SDLAudioPlayer::Play(const uint8_t* buf, size_t len) {
    if (Buffered() < 6000) {
        // Don't fill the audio buffer too much in case this happens
        SDL_QueueAudio(mDevice, buf, len);
    }
}
} // namespace Ship
