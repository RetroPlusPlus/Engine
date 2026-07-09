// Save-store demo — persistence without a window.
//
// A console program that exercises the whole SaveStore surface against the REAL platform
// save directory: it resolves the per-user location from EngineConfig::identity
// (~/Library/Application Support/Retro++/SaveStoreDemo on macOS, %APPDATA% on Windows,
// $XDG_DATA_HOME on Linux), writes a schema-version-1 document, reads it back, then
// declares current version 2 with a registered 1→2 migration step and reads again — the
// document comes back already migrated. The migrated payload is written back at v2, so
// the next run reads it with no migration needed.
//
// Run it twice: the second run finds the first run's document on disk and says so — a
// document surviving the process is the point of the whole subsystem. Every stage prints
// what happened and where the file lives, so you can inspect the document on disk.
//
// The payload here is a plain text string, serialized as raw bytes — standing in for
// whatever bytes a real game would produce. The store never interprets them; the v1→v2
// "format change" below is simply appending a field, and the migration step performs
// that append on old documents.
//
// Headless: no window, no GPU, no audio device. Safe to run anywhere, including over SSH.

#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "retropp/engine_config.h"  // EngineConfig + AppIdentity — who this program is on disk
#include "retropp/save_store.h"     // SaveStore — the durable, versioned byte-document store

namespace {

// The demo's "serialization": text ↔ bytes, one to one. A real game marshals its own
// structs here; the store sees only bytes either way.
std::vector<std::byte> toBytes(const std::string& text) {
    const auto* p = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(p, p + text.size());
}

std::string toText(const std::vector<std::byte>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

constexpr const char* kDocument = "profile";

}  // namespace

int main() {
    // Identify the program to the host OS. This pair names the save directory and is
    // REQUIRED — a default-constructed SaveStore refuses an unset identity, the same way
    // no platform ships an app without one. Set it once and never change it (a changed
    // identity strands the players' documents under the old directory).
    retropp::EngineConfig config;
    config.identity = {.organization = "Retro++", .application = "SaveStoreDemo"};
    retropp::EngineConfig::setActive(config);

    // A default-constructed store resolves the platform directory from the active config.
    retropp::SaveStore store;
    std::printf("save directory: %s\n\n", store.basePath().string().c_str());

    // ── A previous run's document survives the process ─────────────────────────────────
    if (store.exists(kDocument)) {
        std::printf("found \"%s\" from a previous run — persistence across processes.\n",
                    kDocument);
    } else {
        std::printf("no \"%s\" document yet — first run.\n", kDocument);
    }

    // ── Write at schema version 1, read it back ────────────────────────────────────────
    // The write is atomic: temp file + flush + rename. A crash mid-write would leave any
    // previous document untouched — there is never a half-written document on disk.
    const std::string v1 = "name=Ferrym4n";
    if (!store.write(kDocument, 1, toBytes(v1))) {
        std::printf("write failed — cannot continue.\n");
        return 1;
    }
    std::printf("wrote  v1: \"%s\"\n", v1.c_str());

    if (auto doc = store.read(kDocument)) {
        std::printf("read   v%u: \"%s\"\n", doc->schemaVersion, toText(doc->payload).c_str());
    }

    // ── The format grows a field: declare v2 + the 1→2 migration ───────────────────────
    // The step is plain code that knows both layouts: it takes v1 payload bytes and
    // returns v2 payload bytes. The store applies it on read whenever it meets an old
    // document; the demo's v1 document comes back already at v2.
    store.setCurrentVersion(2);
    store.registerMigration(1, [](std::vector<std::byte> old) {
        std::string text = toText(old);
        text += ";playtime=0";  // the new v2 field, defaulted for migrated saves
        return toBytes(text);
    });

    if (auto doc = store.read(kDocument)) {
        std::printf("read   v%u (migrated on load): \"%s\"\n", doc->schemaVersion,
                    toText(doc->payload).c_str());

        // Write the migrated payload back at v2 — the on-disk document is now current,
        // and the next run's read applies no migration.
        if (store.write(kDocument, doc->schemaVersion, doc->payload)) {
            std::printf("wrote  v%u back — the document on disk is current.\n",
                        doc->schemaVersion);
        }
    }

    std::printf("\nrun me again: the v2 document above will still be there.\n");
    return 0;
}
