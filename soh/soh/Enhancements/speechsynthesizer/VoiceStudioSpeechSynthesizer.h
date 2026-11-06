#pragma once

#include "SpeechSynthesizer.h"
#include <string>

// Speaks SoH's TTS text with a VoiceStudio clone voice (OpenAI-compatible API,
// default http://127.0.0.1:3900). Implementation details live in the .cpp.
class VoiceStudioSpeechSynthesizer : public SpeechSynthesizer {
  public:
    VoiceStudioSpeechSynthesizer(std::string voice, std::string host = "127.0.0.1", int port = 3900,
                                 std::string cacheDir = "tts_cache");
    ~VoiceStudioSpeechSynthesizer();

    void Speak(const char* text, const char* language);

  protected:
    bool DoInit(void);
    void DoUninitialize(void);

  private:
    struct Impl;
    Impl* mImpl;
};
