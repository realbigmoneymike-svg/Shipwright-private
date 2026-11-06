#pragma once

#include "SpeechSynthesizer.h"
#include <string>
#include <vector>

// Speaks SoH's TTS text with a VoiceStudio clone voice (OpenAI-compatible API,
// default http://127.0.0.1:3900). Each voice has its own folder: tts_cache/<voice>/.
class VoiceStudioSpeechSynthesizer : public SpeechSynthesizer {
  public:
    VoiceStudioSpeechSynthesizer(std::string voice, std::string host = "127.0.0.1", int port = 3900,
                                 std::string cacheDir = "tts_cache");
    ~VoiceStudioSpeechSynthesizer();

    void Speak(const char* text, const char* language);

    // Change voice while the game runs (used by the voice picker).
    void SetVoice(const std::string& voice);

    // Voice names for the picker: folders in tts_cache plus whatever VoiceStudio last reported.
    static std::vector<std::string> KnownVoices(const std::string& cacheDir = "tts_cache");
    // Ask VoiceStudio for its voice list in the background; KnownVoices() picks it up when it answers.
    static void RefreshServerVoices(std::string host = "127.0.0.1", int port = 3900);
    // The folder name used for a voice (characters Windows can't use become '_').
    static std::string FolderName(const std::string& voice);

  protected:
    bool DoInit(void);
    void DoUninitialize(void);

  private:
    struct Impl;
    Impl* mImpl;
};

// (Re)create SpeechSynthesizer::Instance from the settings: VoiceStudio when
// gSettings.VoiceStudioTTS is on (voice gSettings.VoiceStudioVoice), otherwise the system voice.
void SpeechSynthesizer_ApplySettings(void);
