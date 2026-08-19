// User-files demo — the player's own directory, without a window.
//
// A console program that puts files in the REAL per-user data directory: it resolves the location from
// EngineConfig::identity (~/Library/Application Support/Retro++/UserFilesDemo on macOS, %APPDATA% on
// Windows, $XDG_DATA_HOME on Linux), writes a small tree of "extracted assets" into it, reads one back,
// and points EngineConfig::assetRoot at the result — the shape a game uses when its assets are derived
// from something the player supplied and are never shipped.
//
// It also shows the property the surface exists for: a SaveStore constructed beside the UserFiles
// resolves the SAME directory. One resolution, one owner — a game's other files cannot drift away from
// where its saves live.
//
// Run it twice: the second run finds the first run's files already on disk and says so. Every stage
// prints the absolute path, so you can open the files yourself — they are exactly the bytes written,
// with no envelope in front of them, which is the difference from a save document.
//
// Headless: no window, no GPU, no audio device. Safe to run anywhere, including over SSH.

#include <cstdio>
#include <string>
#include <vector>

#include "retropp/engine_config.h"  // EngineConfig + AppIdentity — who this program is on disk
#include "retropp/save_store.h"     // SaveStore + userDataDir — the directory both stores share
#include "retropp/user_files.h"     // UserFiles — files in that directory, without the document machinery

namespace {

std::vector<std::byte> toBytes(const std::string& text) {
    const auto* p = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(p, p + text.size());
}

std::string toText(const std::vector<std::byte>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// Stands in for whatever a real extractor produces — decoded tile sheets, a palette, a manifest. The
// point is the SHAPE: several files, in directories, written once on first launch.
struct Extracted {
    const char* logicalPath;
    const char* contents;
};

constexpr Extracted kExtracted[] = {
    {"tiles/overworld.bin", "overworld tile sheet bytes"},
    {"tiles/interior.bin", "interior tile sheet bytes"},
    {"palettes/day.bin", "day palette bytes"},
    {"manifest.txt", "3 files extracted"},
};

}  // namespace

int main() {
    // The identity is what names the directory. Both fields are required — an unconfigured program has
    // no correct per-user directory, so the engine refuses rather than inventing a shared one.
    retropp::EngineConfig config;
    config.identity = {.organization = "Retro++", .application = "UserFilesDemo"};
    retropp::EngineConfig::setActive(config);

    // The directory itself, named without constructing a store. Resolving it creates it if absent;
    // nothing inside it is created until something writes.
    std::printf("user data directory: %s\n\n", retropp::userDataDir().string().c_str());

    retropp::UserFiles files;

    // ── First launch vs. every launch after ─────────────────────────────────────────────────────
    // exists() is how a game decides whether the one-time work has already been done. On the second
    // run of this demo the whole extraction is skipped.
    if (files.exists("assets/manifest.txt")) {
        std::printf("assets are already extracted — skipping the extraction\n");
    } else {
        std::printf("first run: extracting assets\n");
        for (const Extracted& item : kExtracted) {
            const std::string logical = std::string("assets/") + item.logicalPath;
            // write() creates the directories on the way, so "assets/tiles/overworld.bin" needs no
            // mkdir of its own. A relative path is the name here — that is what lets a tree exist.
            if (!files.write(logical, toBytes(item.contents))) {
                std::printf("  FAILED to write %s\n", logical.c_str());
                return 1;
            }
            std::printf("  wrote %s\n", files.pathFor(logical).string().c_str());
        }
    }

    // ── Reading back ────────────────────────────────────────────────────────────────────────────
    // An absent file reads as std::nullopt rather than throwing: there is no envelope to validate, so
    // there is no such thing as a corrupt file here — only a present one and an absent one.
    if (const auto manifest = files.read("assets/manifest.txt")) {
        std::printf("\nmanifest says: %s\n", toText(*manifest).c_str());
    }
    if (!files.read("assets/never_written.bin")) {
        std::printf("a file that was never written reads as absent, not as an error\n");
    }

    // ── Handing the directory to the engine ─────────────────────────────────────────────────────
    // root() is the whole reason a game needs this: the assets are on disk, and LoadFromPath has to
    // resolve into them. This is the one-line change — no second path resolution anywhere.
    config.assetRoot = files.root() / "assets";
    retropp::EngineConfig::setActive(config);
    std::printf("\nassetRoot now resolves into: %s\n", config.assetRoot.string().c_str());

    // ── The shared directory ────────────────────────────────────────────────────────────────────
    // The property the surface exists for. Both stores root at the same resolution, so a game's files
    // and its saves cannot end up in different places.
    retropp::SaveStore saves;
    std::printf("\nthe save store resolved: %s\n", saves.basePath().string().c_str());
    std::printf("the file store resolved: %s\n", files.root().string().c_str());
    std::printf("same directory: %s\n", saves.basePath() == files.root() ? "yes" : "NO");

    // ── Containment ─────────────────────────────────────────────────────────────────────────────
    // A path that could resolve outside the store is refused before anything touches the disk — the
    // guarantee that a logical path from a data file or a manifest can never write somewhere else.
    try {
        (void)files.pathFor("assets/../../escaped.bin");
        std::printf("\nescaping path was NOT refused — that is a defect\n");
    } catch (const std::invalid_argument&) {
        std::printf("\n\"assets/../../escaped.bin\" is refused: a file cannot leave the store\n");
    }

    std::printf("\nrun this again — the second run finds these files already on disk\n");
    return 0;
}
