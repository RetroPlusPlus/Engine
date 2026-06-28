// The PCM (audio-file) registration — kept in its own translation unit because it is the one place
// that names the decoder (detail::decodePcm, which pulls in the vendored dr_wav / stb_vorbis). A binary
// links this unit, and the decoder, only when it references the no-ISA registerAudio overload — i.e. only
// when it actually registers an audio file. A chiptune-only program never references it, so the linker
// leaves this unit (and every decoder byte) out. The rest of the AudioLibrary lives in audio_library.cpp,
// which names no decoder and is always linked.
#include "retropp/audio_library.h"

#include <optional>
#include <string>

#include "src/audio/pcm_decode.h"  // detail::decodePcm, detail::g_pcmDecode

namespace retropp {

AudioId AudioLibrary::registerAudio(LiteralPath resourcePath, AudioType type,
                                    std::optional<AssetPolicy> policy) {
    // PCM (audio-file) registration — no ISA: a decoded file runs no code, so ISA (a chiptune-only concept) is
    // meaningless. The kind is inferred from the extension (.wav / .ogg → Pcm); the entry's isa field is
    // inert for a Pcm entry (play() never consults it on a PCM system). Registering an audio file means this
    // program decodes one, so install the decode hook AudioSystem calls through (idempotent — same value
    // every call); this is also the reference that pulls the decoder into the binary.
    detail::g_pcmDecode = &detail::decodePcm;
    entries_.push_back(Entry{
        .kind     = audioKindForExtension(resourcePath.view()),
        .type     = type,
        .isa      = Isa{},  // inert for PCM — ISA is chiptune-only
        .policy   = policy,
        .bytecode = {},
        .asmPath  = std::string(resourcePath.view()),
    });
    return static_cast<AudioId>(entries_.size() - 1);
}

}  // namespace retropp
