# Persistence — the save store

`save_store.h` · `app_identity.h` · `engine_config.h`

`SaveStore` is the engine's durable storage primitive: a store of named **byte documents**,
each tagged with a schema version, written atomically to the platform-correct per-user
directory. Game saves and settings both persist through it. The payload is opaque bytes end
to end — the store never interprets a document's contents; what a save *contains* is
entirely yours.

```cpp
#include "retropp/save_store.h"

retropp::SaveStore store;                       // resolves the platform save directory
store.write("slot1", 1, bytes);                 // atomic: never leaves a partial file
if (auto doc = store.read("slot1")) {           // nullopt = no such document
    load(doc->payload);                         // std::vector<std::byte>, exactly as written
}
```

## Where documents live

A default-constructed `SaveStore` resolves its directory once, at construction, from the
application identity (`AppIdentity` — the program's identity to the host platform,
declared on `EngineConfig::identity`; `setActive()` fans it out to
`SaveStore::defaultIdentity` like the other per-type defaults). Set it in the startup
config before constructing a store:

```cpp
retropp::EngineConfig config;
config.identity = {.organization = "MyStudio", .application = "MyGame"};
retropp::EngineConfig::setActive(config);

retropp::SaveStore store;
store.basePath();   // the resolved directory
```

The identity maps to each platform's conventional per-user data location:

| Platform | Directory |
|---|---|
| Windows | `%APPDATA%\MyStudio\MyGame\` |
| macOS | `~/Library/Application Support/MyStudio/MyGame/` |
| Linux | `$XDG_DATA_HOME/MyStudio/MyGame/` (or `~/.local/share/...`) |

The identity is deliberately **required, with no defaults** — `setActive()` itself refuses a
config with either field empty, and a default-constructed `SaveStore` throws `SaveStoreError`
when the identity it resolves against is unset. There is no fallback name —
a fallback would silently resolve every unconfigured game to the same directory and their
saves would collide, so an unset identity refuses loudly on first run instead. Set both
fields once, and never change them after players have saves: a changed identity is a
different directory, and the players' documents stay stranded under the old one.

To root a store somewhere else entirely — a test fixture, a portable-install layout, a
second profile — bypass the platform resolution with an explicit base:

```cpp
auto store = retropp::SaveStore::atPath("/some/explicit/directory");
```

Two stores at different bases are fully independent; the store holds no global state. The
directory does not need to exist yet — it is created on first write. This is also how tests
stay hermetic: every store test roots at a unique directory under the system temp path and
removes it afterwards, so nothing touches real save data and no test sees another's files.

## Document names

A document name is a flat identifier — `"slot1"`, `"settings"`, `"profile"` — not a path.
Names containing path separators or a drive designator, and the names `"."` / `".."`, throw
`std::invalid_argument`: a document can never land outside the store's directory.

## Writes are atomic

`write` puts the document on disk in a way a crash cannot corrupt:

1. The envelope and payload are written to a sibling temp file in the same directory.
2. The temp file is flushed to the device (the OS is asked to write it through, not
   merely accept it into a buffer).
3. The temp file is moved over the target in one filesystem rename — the single commit
   point.

A crash before the rename leaves the previous document exactly as it was; a crash after it
leaves the new document complete. There is no moment at which the named document is partial.
A failed `write` returns `false` with the prior document untouched and no temp debris left
behind.

This is why saving through the store needs no "backup the old file first" ritual — the
prior version *is* the fallback until the instant the new one is durably in place.

## Absent is not corrupt

The two failure signals are deliberately different, because they demand different responses:

- **Absent** — `read` returns `std::nullopt`. The ordinary first-run case; proceed with
  defaults.
- **Untrustworthy** — `read` throws `SaveStoreError`: the file is not a save document (bad
  magic tag), it is truncated, its container format is unknown, its schema version is newer
  than the running code declares, or its migration chain has a gap.

A corrupt document never reads as "no document." If it did, the natural caller response —
start fresh, write defaults — would silently overwrite the player's damaged-but-maybe-
recoverable data. Catch `SaveStoreError` where you read, and decide deliberately.

## Schema versions and migration

Every `write` tags the document with **your** schema version — a plain `uint32` whose
meaning belongs to you. When your format changes, bump the version and register one step
per bump; each step is plain code that transforms version-`v` payload bytes into
version-`v+1` payload bytes:

```cpp
store.setCurrentVersion(3);
store.registerMigration(1, upgradeV1ToV2);   // your function: v1 bytes in, v2 bytes out
store.registerMigration(2, upgradeV2ToV3);

auto doc = store.read("slot1");
// A v1 file on disk arrives here as doc->payload at v3, walked 1→2→3 in order.
```

The store orchestrates; it never interprets. On `read`:

- stored version **==** current — returned as-is, no steps run.
- stored version **<** current — walked through the registered steps in sequence and
  returned at the current version. Write it back at the current version when convenient;
  until you do, the old file just migrates again on each read.
- stored version **>** current — `SaveStoreError`. The file came from a newer build than
  the running code; guessing at it risks destroying it.
- A **gap** in the chain (current is 3, only `1→2` registered, a v1 file arrives) —
  `SaveStoreError`. A half-migrated document is worse than a refused one.

With no current version declared, `read` returns every document verbatim at its stored
version — the caller owns versioning entirely.

Because each step covers exactly one version bump, an ancient document upgrades through
every intermediate format with no combinatorial `1→N` transforms — one function per format
change you ever made, written once, when you made it.

## The envelope

On disk, a document is a small fixed header followed by the payload bytes: a magic tag
(`RPSV`), the store's own container-format version, your schema version, and the payload
length. All header fields are little-endian regardless of platform, so a document written
on one platform reads on any other. The header is validated on every read; the payload is
handed back untouched.

## The full surface

| Call | Does |
|---|---|
| `SaveStore()` | Store at the platform directory resolved from the application identity (`setActive()` seeds `SaveStore::defaultIdentity` from `EngineConfig::identity`); throws if the identity is unset |
| `SaveStore::defaultIdentity` | The `AppIdentity` a default-constructed store resolves against — normally seeded by `setActive()`; assign directly only when bypassing the config bundle |
| `SaveStore::atPath(dir)` | Store rooted at an explicit directory |
| `write(name, version, bytes)` | Atomic write/replace; `false` on failure with the prior document intact |
| `read(name)` | `std::optional<Document>`; `nullopt` if absent; throws `SaveStoreError` if untrustworthy; migrates if older |
| `setCurrentVersion(v)` | Declare the schema version the running code is written against |
| `registerMigration(from, step)` | Register the `from → from+1` payload transform |
| `exists(name)` / `remove(name)` | Presence check / delete |
| `basePath()` | The resolved directory |

`Document` carries `schemaVersion` (the version the payload is *at* — post-migration when
steps ran) and `payload` (`std::vector<std::byte>`).

## Try it

`examples/save_store_demo/` is a headless console program covering the whole surface
against the real platform directory: it writes a v1 document, reads it back, then declares
v2 with a `1→2` migration and reads the same file already migrated. Run it twice — the
second run finds the first run's document, which is the point of the whole subsystem.

## Related pages & where to change things

- [platform-and-windowing.md](platform-and-windowing.md) — the `EngineConfig` startup
  bundle and `setActive()`, which carries the identity to the store's default.
- The platform write path (temp + flush + atomic rename, per-OS) lives in
  `src/save_store.cpp`; the envelope layout and migration walk live there too. The store's
  public contract is `include/retropp/save_store.h`; the identity type is
  `include/retropp/app_identity.h`.
