// The audio-pack backend demo (dev-run only; never auto-launched, never run in CI).
//
// The engine's second audio backend plays user audio FILES, not the chiptune VM. A WAV and an OGG are
// registered on the AudioLibrary and cued through a PCM AudioSystem (AudioKind::Pcm) with play() — exactly
// as a chiptune is cued. Each file's AudioId is tagged Pcm (by extension), so play() decodes it through
// the file decoder (dr_wav / stb_vorbis); a PCM system has no VM at all. Audio only — no window, no GPU —
// so it is photosensitivity-safe by construction.
//
// The two files exercise both asset-delivery policies: chime.wav ships beside the binary (LoadFromPath,
// the per-type default for an audio file) and is decoded from disk; blip.ogg is baked into the binary
// (the opt-in AssetPolicy::Embed) and decoded from memory. The 44.1 kHz chime is resampled to the 48 kHz
// device rate on decode.
#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <thread>

#include "retropp/asset_policy.h"
#include "retropp/audio_library.h"
#include "retropp/audio_system.h"
#include "retropp/engine_config.h"  // EngineConfig::setActive — roots assetRoot at the executable dir
#include "retropp/isa.h"

int main() {
    // Audio only — no video subsystem, so no window is ever created.
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init(AUDIO) failed: %s\n", SDL_GetError());
        return 1;
    }
    // Root LoadFromPath asset resolution at the executable directory, where the build copied chime.wav.
    retropp::EngineConfig::setActive(retropp::EngineConfig{});

    const retropp::AudioId chime = retropp::AudioLibrary::instance().registerAudio(
        "examples/audio_pack_demo/assets/chime.wav", retropp::AudioType::Sfx, retropp::AssetPolicy::LoadFromPath);
    const retropp::AudioId blip = retropp::AudioLibrary::instance().registerAudio(
        "examples/audio_pack_demo/assets/blip.ogg", retropp::AudioType::Sfx, retropp::AssetPolicy::Embed);

    {
        // A PCM system — owns its own SdlAudioSink (48 kHz device) and has no VM.
        retropp::AudioSystem audio{retropp::AudioKind::Pcm};

        std::puts("Playing chime.wav (WAV, decoded from disk, resampled 44.1 -> 48 kHz)...");
        audio.play(chime);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));

        std::puts("Playing blip.ogg (OGG Vorbis, decoded from embedded bytes)...");
        audio.play(blip);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));

        std::printf("Done. dropped=%zu underflow=%zu\n", audio.framesDropped(),
                    audio.underflowFrames());
    }  // the AudioSystem stops the sink and tears down here

    SDL_Quit();
    return 0;
}
