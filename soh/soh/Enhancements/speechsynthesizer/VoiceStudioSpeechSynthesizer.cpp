// VoiceStudioSpeechSynthesizer.cpp
// Flow: text -> tts_cache/<voice>/<fnv1a(voice|text)>.wav if present -> else POST to
// VoiceStudio /v1/audio/speech, save to that folder -> play on its own SDL device.

#include "httplib.h" // keep first: must come before anything that pulls in windows.h
#include "VoiceStudioSpeechSynthesizer.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <nlohmann/json.hpp>
#include "soh/cvar_prefixes.h"
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

namespace fs = std::filesystem;

namespace {
// UTF-8 <-> path (so voice names with accents work as folder names on Windows)
fs::path U8Path(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}
std::string PathU8(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::mutex sServerVoicesMutex;
std::vector<std::string> sServerVoices;
std::atomic<bool> sRefreshing{ false };

// Collects voice names from whatever shape VoiceStudio's voice list comes back in.
void CollectNames(const nlohmann::json& j, std::vector<std::string>& out, int depth = 0) {
    if (depth > 3) {
        return;
    }
    if (j.is_array()) {
        for (auto& e : j) {
            if (e.is_string()) {
                out.push_back(e.get<std::string>());
            } else if (e.is_object()) {
                for (const char* key : { "name", "voice", "voice_name", "id" }) {
                    if (e.contains(key) && e[key].is_string()) {
                        out.push_back(e[key].get<std::string>());
                        break;
                    }
                }
            }
        }
    } else if (j.is_object()) {
        for (const char* key : { "voices", "data", "profiles", "items", "results" }) {
            if (j.contains(key)) {
                CollectNames(j[key], out, depth + 1);
            }
        }
    }
}
} // namespace

std::string VoiceStudioSpeechSynthesizer::FolderName(const std::string& voice) {
    std::string out;
    for (unsigned char c : voice) {
        out += (c < 0x20 || std::string("<>:\"/\\|?*").find((char)c) != std::string::npos) ? '_' : (char)c;
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
        out.pop_back();
    }
    return out.empty() ? "_" : out;
}

struct VoiceStudioSpeechSynthesizer::Impl {
    std::string voice, host;
    int port;
    fs::path cacheDir;

    std::thread worker;
    std::mutex mutex; // guards queue and voice
    std::condition_variable cv;
    std::deque<std::string> queue;
    std::atomic<bool> running{ false }, interrupt{ false };

    SDL_AudioDeviceID device = 0;
    SDL_AudioSpec spec{};

    static std::string Hash(const std::string& voice, const std::string& text) {
        // FNV-1a 64-bit of "<voice>|<text>", same as prerender_soh_tts.py
        uint64_t h = 0xCBF29CE484222325ULL;
        for (unsigned char c : voice + "|" + text) {
            h ^= c;
            h *= 0x100000001B3ULL;
        }
        return std::to_string(h) + ".wav";
    }

    std::string GetWav(const std::string& voice, const std::string& text) {
        std::error_code ec;
        fs::path dir = cacheDir / U8Path(FolderName(voice));
        std::string file = Hash(voice, text);
        fs::path p = dir / file;

        fs::path legacy = cacheDir / file; // older builds kept every voice in one folder
        if (!fs::exists(p, ec) && fs::exists(legacy, ec)) {
            fs::create_directories(dir, ec);
            fs::rename(legacy, p, ec);
            if (ec) {
                p = legacy;
            }
        }
        if (fs::exists(p, ec)) {
            std::ifstream f(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)), {});
        }
        if (fs::exists(p.string() + ".part", ec)) {
            return {}; // the pre-render script is writing this one right now
        }

        httplib::Client cli(host, port);
        cli.set_read_timeout(30);
        nlohmann::json body = { { "model", "tts-1" }, { "voice", voice }, { "input", text },
                                { "response_format", "wav" } };
        // replace: SoH can pass bytes that aren't valid UTF-8 (e.g. 0x80), which would otherwise throw
        auto res = cli.Post("/v1/audio/speech", body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
                            "application/json");
        if (!res || res->status != 200) {
            return {}; // VoiceStudio not running or voice name wrong -> stay silent
        }
        fs::create_directories(dir, ec);
        fs::path tmp = p.string() + ".game";
        std::ofstream(tmp, std::ios::binary) << res->body;
        fs::rename(tmp, p, ec); // never leave a half-written WAV in the cache
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
            std::string line, v;
            {
                std::unique_lock<std::mutex> lk(mutex);
                cv.wait(lk, [&] { return !queue.empty() || !running; });
                if (!running) {
                    break;
                }
                line = std::move(queue.front());
                queue.pop_front();
                v = voice;
                interrupt = false;
            }
            std::string wav = GetWav(v, line);
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
    mImpl->cacheDir = U8Path(cacheDir);
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

void VoiceStudioSpeechSynthesizer::SetVoice(const std::string& voice) {
    std::lock_guard<std::mutex> lk(mImpl->mutex);
    mImpl->voice = voice;
}

std::vector<std::string> VoiceStudioSpeechSynthesizer::KnownVoices(const std::string& cacheDir) {
    std::set<std::string> names;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(U8Path(cacheDir), ec)) {
        if (e.is_directory(ec)) {
            names.insert(PathU8(e.path().filename()));
        }
    }
    {
        std::lock_guard<std::mutex> lk(sServerVoicesMutex);
        names.insert(sServerVoices.begin(), sServerVoices.end());
    }
    return { names.begin(), names.end() };
}

void VoiceStudioSpeechSynthesizer::RefreshServerVoices(std::string host, int port) {
    if (sRefreshing.exchange(true)) {
        return;
    }
    std::thread([host, port] {
        std::vector<std::string> found;
        httplib::Client cli(host, port);
        cli.set_connection_timeout(2);
        cli.set_read_timeout(5);
        for (const char* path : { "/v1/voices", "/voices", "/api/voices", "/profiles", "/api/profiles" }) {
            auto res = cli.Get(path);
            if (!res || res->status != 200) {
                continue;
            }
            auto j = nlohmann::json::parse(res->body, nullptr, false);
            if (!j.is_discarded()) {
                CollectNames(j, found);
            }
            if (!found.empty()) {
                break;
            }
        }
        if (!found.empty()) {
            std::lock_guard<std::mutex> lk(sServerVoicesMutex);
            sServerVoices = std::move(found);
        }
        sRefreshing = false;
    }).detach();
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

void SpeechSynthesizer_ApplySettings(void) {
    bool wantVoiceStudio = CVarGetInteger(CVAR_SETTING("VoiceStudioTTS"), 0) != 0;
    std::string voice = CVarGetString(CVAR_SETTING("VoiceStudioVoice"), "default");

    if (auto* vs = dynamic_cast<VoiceStudioSpeechSynthesizer*>(SpeechSynthesizer::Instance)) {
        if (wantVoiceStudio) {
            vs->SetVoice(voice); // same backend, just a different voice: no restart needed
            return;
        }
    } else if (SpeechSynthesizer::Instance != nullptr && !wantVoiceStudio) {
        return; // already on the system voice
    }

    if (SpeechSynthesizer::Instance != nullptr) {
        SpeechSynthesizer::Instance->Uninitialize();
        delete SpeechSynthesizer::Instance;
        SpeechSynthesizer::Instance = nullptr;
    }

    if (wantVoiceStudio) {
        SpeechSynthesizer::Instance = new VoiceStudioSpeechSynthesizer(voice);
    } else {
#ifdef __APPLE__
        SpeechSynthesizer::Instance = new DarwinSpeechSynthesizer();
#elif defined(_WIN32)
        SpeechSynthesizer::Instance = new SAPISpeechSynthesizer();
#elif ESPEAK
        SpeechSynthesizer::Instance = new ESpeakSpeechSynthesizer();
#else
        SpeechSynthesizer::Instance = new SpeechLogger();
#endif
    }
    SpeechSynthesizer::Instance->Init();
}
