// AudioLibrary implementation: a pure-data catalog of audio definitions, no VM dependency.
#include "retropp/audio_library.h"

#include <vector>

#include "src/audio/pcm_decode.h"  // detail::g_pcmDecode — declared here, installed by the audio-file door

namespace retropp {

// The decode hook lives in this always-linked, decoder-free translation unit so AudioSystem can resolve it
// without naming decodePcm (and so dragging the decoder into every audio binary). It stays null until the
// audio-file registration door (audio_library_pcm.cpp) installs it; a chiptune-only program never does.
namespace detail {
PcmDecodeFn g_pcmDecode = nullptr;
}  // namespace detail

// audioKindForExtension (and its endsWith helper) are constexpr in audio_library.h — one predicate for both
// the runtime kind inference and the compile-time door check on ChiptunePath.

AudioLibrary& AudioLibrary::instance() {
    // Function-local static: constructed on first use (lean — unreferenced ⇒ not linked), destroyed at
    // program exit, thread-safe initialization. The single instance the header guarantees.
    static AudioLibrary library;
    return library;
}

AudioId AudioLibrary::uploadAudio(std::span<const std::uint8_t> bytecode, AudioType type, Isa isa) {
    entries_.push_back(Entry{
        .kind     = AudioKind::Chiptune,
        .type     = type,
        .isa      = isa,
        .policy   = {},  // raw bytes carry no embed/load policy — you brought the bytes
        .bytecode = std::vector<std::uint8_t>(bytecode.begin(), bytecode.end()),  // owned copy
        .asmPath  = {},
    });
    return static_cast<AudioId>(entries_.size() - 1);
}

// The ISA (chiptune) registerAudio overload is a constrained template defined in audio_library.h (the
// constraint gates candidacy so a no-ISA PCM call resolves to the no-ISA door without tripping the
// ChiptunePath compile-time check).
//
// The no-ISA registerAudio overload (the PCM / audio-file door) is defined in audio_library_pcm.cpp. That
// translation unit is the only one that names the decoder, so a binary links it — and the decoder — only
// when it actually registers an audio file.

const AudioLibrary::Entry& AudioLibrary::entry(AudioId id) const {
    return entries_[static_cast<std::size_t>(id)];
}

}  // namespace retropp
