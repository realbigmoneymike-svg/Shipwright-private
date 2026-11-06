// VoiceStudioSpeechSynthesizer.cpp
// Flow: text -> tts_cache/<fnv1a(voice|text)>.wav if present -> else POST to
// VoiceStudio /v1/audio/speech, save to cache -> play on its own SDL device.

#include "httplib.h" // keep first: must come before anything that pulls in windows.h
#include "VoiceStudioSpeechSynthesizer.h"

#include <nlohmann/json.hpp>
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

struct VoiceStudioSpeechSynthesizer::Impl {
    std::string voice, host;
    int port;
    fs::path cacheDir;

    std::thread worker;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::string> queue;
    std::atomic<bool> running{ false }, interrupt{ false };

    SDL_AudioDeviceID device = 0;
    SDL_AudioSpec spec{};

    fs::path CachePath(const std::string& text) const {
        // FNV-1a 64-bit, same as prerender_tts.py
        uint64_t h = 0xCBF29CE484222325ULL;
        for (unsigned char c : voice + "|" + text) {
            h ^= c;
            h *= 0x100000001B3ULL;
        }
        return cacheDir / (std::to_string(h) + ".wav");
    }

    std::string GetWav(const std::string& text) {
        fs::path p = CachePath(text);
        if (fs::exists(p)) {
            std::ifstream f(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)), {});
        }
        httplib::Client cli(host, port);
        cli.set_read_timeout(30);
        nlohmann::json body = { { "model", "tts-1" }, { "voice", voice }, { "input", text },
                                { "response_format", "wav" } };
        auto res = cli.Post("/v1/audio/speech", body.dump(), "application/json");
        if (!res || res->status != 200) {
            return {}; // VoiceStudio not running or voice name wrong -> stay silent
        }
        std::ofstream(p, std::ios::binary) << res->body;
        return res->body;
    }

    void Play(const std::string& wav) {
        SDL_AudioSpec want;
        Uint8* buf = nullptr;
        Uint32 len = 0;
        SDL_RWops* rw = SDL_RWFromConstMem(wav.data(), (int)wav.size());
        if (!SDL_LoadWAV_RW(rw, 1, &want, &buf, &len)) {
            return;
        }
        if (device && (want.freq != spec.freq || want.format != spec.format || want.channels != spec.channels)) {
            SDL_CloseAudioDevice(device);
            device = 0;
        }
        if (!device) {
            want.callback = nullptr; // queue mode
            device = SDL_OpenAudioDevice(nullptr, 0, &want, &spec, 0);
            if (!device) {
                SDL_FreeWAV(buf);
                return;
            }
        }
        SDL_ClearQueuedAudio(device);
        SDL_QueueAudio(device, buf, len);
        SDL_FreeWAV(buf);
        SDL_PauseAudioDevice(device, 0);

        while (SDL_GetQueuedAudioSize(device) > 0 && running) {
            if (interrupt) { // a newer line arrived: cut this one off
                SDL_ClearQueuedAudio(device);
                break;
            }
            SDL_Delay(20);
        }
    }

    void Worker() {
        while (running) {
            std::string line;
            {
                std::unique_lock<std::mutex> lk(mutex);
                cv.wait(lk, [&] { return !queue.empty() || !running; });
                if (!running) {
                    break;
                }
                line = std::move(queue.front());
                queue.pop_front();
                interrupt = false;
            }
            std::string wav = GetWav(line);
            if (!wav.empty() && !interrupt) {
                Play(wav);
            }
        }
    }
};

VoiceStudioSpeechSynthesizer::VoiceStudioSpeechSynthesizer(std::string voice, std::string host, int port,
                                                           std::string cacheDir)
    : mImpl(new Impl()) {
    mImpl->voice = std::move(voice);
    mImpl->host = std::move(host);
    mImpl->port = port;
    mImpl->cacheDir = std::move(cacheDir);
}

VoiceStudioSpeechSynthesizer::~VoiceStudioSpeechSynthesizer() {
    Uninitialize();
    delete mImpl;
}

void VoiceStudioSpeechSynthesizer::Speak(const char* text, const char* /*language*/) {
    if (!text || !*text) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mImpl->mutex);
        mImpl->queue.clear();
        mImpl->queue.emplace_back(text);
        mImpl->interrupt = true;
    }
    mImpl->cv.notify_one();
}

bool VoiceStudioSpeechSynthesizer::DoInit(void) {
    std::error_code ec;
    fs::create_directories(mImpl->cacheDir, ec);
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        return false;
    }
    mImpl->running = true;
    mImpl->worker = std::thread(&Impl::Worker, mImpl);
    return true;
}

void VoiceStudioSpeechSynthesizer::DoUninitialize(void) {
    mImpl->running = false;
    mImpl->cv.notify_one();
    if (mImpl->worker.joinable()) {
        mImpl->worker.join();
    }
    if (mImpl->device) {
        SDL_CloseAudioDevice(mImpl->device);
        mImpl->device = 0;
    }
}
