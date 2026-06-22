// AudioLibrary implementation: a pure-data catalog of audio definitions, no VM dependency.
#include "retropp/audio_library.h"

#include <string>
#include <vector>

namespace retropp {

namespace {
// Case-sensitive suffix match (logical asset paths are authored lowercase). Small + allocation-free.
bool endsWith(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
}  // namespace

AudioKind audioKindForExtension(std::string_view path) noexcept {
    if (endsWith(path, ".wav") || endsWith(path, ".ogg") || endsWith(path, ".flac") ||
        endsWith(path, ".mp3")) {
        return AudioKind::Pcm;
    }
    return AudioKind::Chiptune;  // `.asm` and anything else — the only realized kind in v1
}

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

AudioId AudioLibrary::registerAudio(LiteralPath resourcePath, AudioType type, Isa isa,
                                    std::optional<AssetPolicy> policy) {
    entries_.push_back(Entry{
        .kind     = audioKindForExtension(resourcePath.view()),
        .type     = type,
        .isa      = isa,
        .policy   = policy,
        .bytecode = {},
        .asmPath  = std::string(resourcePath.view()),
    });
    return static_cast<AudioId>(entries_.size() - 1);
}

const AudioLibrary::Entry& AudioLibrary::entry(AudioId id) const {
    return entries_[static_cast<std::size_t>(id)];
}

}  // namespace retropp
