// AudioLibrary implementation: a pure-data catalog of audio definitions, no VM dependency.
#include "retropp/audio_library.h"

#include <utility>
#include <vector>

#include "src/audio/pcm_decode.h"  // detail::g_pcmDecode — declared here, installed by the no-ISA registerAudio

namespace retropp {

// The decode hook lives in this always-linked, decoder-free translation unit so AudioSystem can resolve it
// without naming decodePcm (and so dragging the decoder into every audio binary). It stays null until the
// no-ISA registerAudio (audio_library_pcm.cpp) installs it; a chiptune-only program never does.
namespace detail {
PcmDecodeFn g_pcmDecode = nullptr;
}  // namespace detail

// audioKindForExtension (and its endsWith helper) are constexpr in audio_library.h — one predicate for both
// the runtime kind inference and the compile-time check on ChiptunePath.

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
// constraint gates candidacy so a no-ISA PCM call resolves to the no-ISA overload without tripping the
// ChiptunePath compile-time check).
//
// The no-ISA registerAudio overload (PCM / audio files) is defined in audio_library_pcm.cpp. That
// translation unit is the only one that names the decoder, so a binary links it — and the decoder — only
// when it actually registers an audio file.

DriverDefinition AudioLibrary::lowerMachine(Mapper mapper, std::uint32_t tickEntry,
                                            std::optional<std::uint32_t> stackTop,
                                            std::optional<Instruction> init) {
    DriverDefinition def;
    def.mapper    = mapper;
    def.tickEntry = tickEntry;
    def.stackTop  = stackTop;
    def.init      = std::move(init);
    return def;
}

AudioId AudioLibrary::storeDriver(DriverDefinition def, Isa isa) {
    entries_.push_back(Entry{
        .kind     = AudioKind::Driver,
        .type     = AudioType::VMDriver,  // a driver rides the VMDriver bus by construction — a straight
                                          // amplifier over its own authentic internal mixing; never cued
                                          // onto a cue bus (it is hosted, not play()'d)
        .isa      = isa,
        .policy   = {},
        .bytecode = {},
        .asmPath  = {},
        .driver   = std::move(def),
    });
    return static_cast<AudioId>(entries_.size() - 1);
}

const AudioLibrary::Entry& AudioLibrary::entry(AudioId id) const {
    return entries_[static_cast<std::size_t>(id)];
}

}  // namespace retropp
