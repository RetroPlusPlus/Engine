// ENG-4.A — the audio tone demo (dev-run only; never auto-launched, never run in CI).
//
// The end-to-end de-risk for the engine audio chain: an AudioSystem (which owns a hidden VM hosting the
// sound driver) plays the built-in gentle diagnostic tone (a ~250 Hz triangle) through a real SDL audio
// device for a few seconds. If SameBoy's APU, the hardware-speed throttle, and the SDL sink agree on
// sample rate, the tone sounds at a steady, correct pitch with no glitches. Audio only — no window, no
// GPU — so it is photosensitivity-safe by construction, and the tone is low-pitch / low-harmonic.
//
// Note how little the "game" sees: construct an AudioSystem, register an audio, play it, tick it. No
// Vm, no Routine, no throttle, no APU register — the audio system handles all of that internally.
#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <thread>

#include "retropp/audio_system.h"
#include "retropp/gb_audio.h"      // retropp::sameboy::diagnosticTone
#include "retropp/sdl_platform.h"  // retropp::SdlAudioSink

int main() {
    // Audio only — no video subsystem, so no window is ever created.
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init(AUDIO) failed: %s\n", SDL_GetError());
        return 1;
    }

    int result = 0;
    {
        retropp::SdlAudioSink sink;            // a real device stream (opened when the system starts)
        retropp::AudioSystem  audio{sink};     // Game Boy Color, 48 kHz — owns its VM internally
        const retropp::AudioId tone = retropp::sameboy::diagnosticTone();
        audio.play(tone);

        std::puts("Playing a ~512 Hz square tone for ~3 seconds (audio only, no window)...");

        // Production runs on the AudioSystem's own thread (ENG-4.D.1) — it self-paces to the device's
        // 48 kHz drain. The demo just cues (above) and waits; it never steps the audio itself.
        std::this_thread::sleep_for(std::chrono::seconds(3));

        std::printf("Done. dropped=%zu underflow=%zu\n",
                    audio.framesDropped(), audio.underflowFrames());
        if (audio.framesDropped() != 0) {
            std::puts("WARNING: frames were dropped — the ring overflowed (tick pacing too fast?).");
        }
    }  // the AudioSystem stops the sink and tears down its VM here

    SDL_Quit();
    return result;
}
