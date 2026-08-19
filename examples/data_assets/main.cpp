// Data-assets demo — the third ingestible family: bytes the engine stores and never interprets.
//
// A console program that registers three data assets, one per delivery, and then decodes them. The engine
// has no idea any of this is text: it hands back spans, and every byte's meaning is decided here.
//
//   * The CHARACTER TABLE is the game's own authored data, so it is registered with AssetPolicy::Embed
//     and the build bakes it into the executable. Nothing for it is on disk beside the binary.
//   * The CORPUS stands in for content derived from something the player supplied — the case this
//     family's default exists for. It is written into the player's own directory on first run, and
//     registered with NO policy argument, so it takes the per-type default (LoadFromPath) and the build
//     bakes nothing. Point EngineConfig::assetRoot at that directory and the logical path resolves into
//     it.
//   * The STRING INDEX is computed here at runtime and handed over as ready bytes with uploadData. There
//     is no file, so there is no policy — the build has nothing to bake or copy.
//
// Run it twice: the second run finds the corpus already extracted, exactly as a real game would.
//
// Headless: no window, no GPU, no audio device. Safe to run anywhere, including over SSH.

#include <cstdio>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "retropp/asset_policy.h"   // AssetPolicy — Embed vs LoadFromPath, written at the call site
#include "retropp/data_library.h"   // DataLibrary + DataId — the family this demo is about
#include "retropp/engine_config.h"  // EngineConfig — identity, and where LoadFromPath resolves
#include "retropp/user_files.h"     // UserFiles — the player's own directory, where the corpus lands

namespace {

// The corpus encoding, which is entirely this program's invention. Each byte indexes the character
// table; 0xFF ends a string. A real port's encoding is whatever its source material used — the engine
// never sees any of it.
constexpr std::uint8_t kEnd = 0xFF;

// "HELLO WORLD!" and "DATA ASSETS ARE BYTES." in that encoding. This is what an extractor would have
// produced from the player's own copy; the demo writes it so nothing here comes from anyone's ROM.
constexpr std::uint8_t kCorpus[] = {
    7, 4, 11, 11, 14, 26, 22, 14, 17, 11, 3, 29, kEnd,
    3, 0, 19, 0, 26, 0, 18, 18, 4, 19, 18, 26, 0, 17, 4, 26, 1, 24, 19, 4, 18, 27, kEnd,
};

std::vector<std::byte> asBytes(std::span<const std::uint8_t> bytes) {
    const auto* p = reinterpret_cast<const std::byte*>(bytes.data());
    return std::vector<std::byte>(p, p + bytes.size());
}

// The whole point of the family: interpretation lives here, not in the engine. One span of encoded
// symbols plus one span of characters becomes the strings this program wanted.
std::vector<std::string> decode(std::span<const std::uint8_t> corpus,
                                std::span<const std::uint8_t> charmap) {
    std::vector<std::string> strings;
    std::string              current;
    for (const std::uint8_t symbol : corpus) {
        if (symbol == kEnd) {
            strings.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(symbol < charmap.size() ? static_cast<char>(charmap[symbol]) : '?');
    }
    return strings;
}

}  // namespace

int main() {
    // The identity names the player's directory. Both fields are required — an unconfigured program has
    // no correct per-user directory, so the engine refuses rather than inventing a shared one.
    retropp::EngineConfig config;
    config.identity = {.organization = "Retro++", .application = "DataAssetsDemo"};
    retropp::EngineConfig::setActive(config);

    retropp::DataLibrary& library = retropp::DataLibrary::instance();

    // ── The table: authored here, baked into the binary ─────────────────────────────────────────
    // An explicit AssetPolicy::Embed token at the call site. Write it as a literal — the build's scan
    // reads the token out of the source text, so a policy passed through a variable is invisible to it
    // and the file is copied instead of baked.
    const retropp::DataId charmapId =
        library.registerData("examples/data_assets/assets/charmap.bin", retropp::AssetPolicy::Embed);

    // ── The corpus: extracted into the player's directory ───────────────────────────────────────
    // A game does this once, from whatever the player supplied. Here the demo writes the bytes itself so
    // the example depends on nothing but itself.
    retropp::UserFiles files;
    if (files.exists("corpus.bin")) {
        std::printf("corpus already extracted\n");
    } else {
        std::printf("first run: extracting the corpus\n");
        if (!files.write("corpus.bin", asBytes(kCorpus))) {
            std::printf("  FAILED to write the corpus\n");
            return 1;
        }
    }
    std::printf("  corpus file: %s\n", files.pathFor("corpus.bin").string().c_str());

    // The one line that connects the two features: LoadFromPath resolves against assetRoot, so pointing
    // it at the player's directory is what makes "corpus.bin" find the extracted file.
    config.assetRoot = files.root();
    retropp::EngineConfig::setActive(config);
    std::printf("  assetRoot resolves into: %s\n\n", config.assetRoot.string().c_str());

    // No policy argument — the per-type default is LoadFromPath, so the build bakes nothing for this
    // path. That default is the family's legal posture: content a game may not redistribute must never
    // end up inside a shipped binary because a policy was left off.
    const retropp::DataId corpusId = library.registerData("corpus.bin");

    // ── Resolving ───────────────────────────────────────────────────────────────────────────────
    // Both handles resolve the same way from here; only where the bytes came from differs. An entry is
    // read once and held for the life of the program, so these spans stay valid.
    const std::span<const std::uint8_t> charmap = library.data(charmapId);
    const std::span<const std::uint8_t> corpus  = library.data(corpusId);
    std::printf("character table: %zu bytes, baked into this executable\n", charmap.size());
    std::printf("corpus:          %zu bytes, read from the player's directory\n", corpus.size());

    // ── The third form: bytes with no file at all ───────────────────────────────────────────────
    // Where each string starts, computed from the corpus just resolved. uploadData copies it, so the
    // local vector can go out of scope and the handle still answers.
    std::vector<std::uint8_t> offsets;
    offsets.push_back(0);
    for (std::size_t i = 0; i + 1 < corpus.size(); ++i) {
        if (corpus[i] == kEnd) {
            offsets.push_back(static_cast<std::uint8_t>(i + 1));
        }
    }
    const retropp::DataId indexId = library.uploadData(offsets);
    std::printf("string index:    %zu bytes, built at runtime and handed over\n\n",
                library.data(indexId).size());

    // ── Decoding ────────────────────────────────────────────────────────────────────────────────
    // The engine delivered three byte spans and stopped there. Everything below is this program's.
    const std::vector<std::string> strings = decode(corpus, charmap);
    const std::span<const std::uint8_t> index = library.data(indexId);
    for (std::size_t i = 0; i < strings.size(); ++i) {
        const unsigned at = i < index.size() ? index[i] : 0u;
        std::printf("  [%zu] at corpus offset %2u: \"%s\"\n", i, at, strings[i].c_str());
    }

    std::printf("\n%zu data assets registered\n", library.size());
    return 0;
}
