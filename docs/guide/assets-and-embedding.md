# Assets — embed vs. load-from-path

Everything the engine ingests from a file — an atlas image (`loadAtlas`), a map PNG (`loadMapPng`), a
chiptune or PCM track (`registerAudio`), a VM routine (`registerRoutine`) — is delivered one of two ways,
and **the choice lives in your code, not in the build system**:

- **Embed** — the bytes are baked into the executable at build time and decoded from memory at runtime.
  The source file never ships. Use it for self-contained binaries, art you don't want altered, build-time
  design inputs (a tilemap's index PNG), and small clean-room code (a chiptune driver, a VM routine).
- **LoadFromPath** — the file ships beside the binary (or is extracted there) and is read from disk at
  runtime. Use it for moddable assets, large streamed media, and — importantly — for anything that must
  **never** be baked into a shipped binary (copyright-derived art or a routine a port populates at runtime
  from the user's own ROM).

You write the ingest call with a policy; the build does the rest automatically — it **bakes** the Embed
inputs into the binary and **copies** the LoadFromPath ones next to it. There is no build rule to write,
no copy step to add, and no path to construct.

## Two doors per family

Each ingestible family has the same pair of doors. They differ only in what you hand over:

| Family | Path door (policy-governed) | Bytes door (you brought the bytes) |
|---|---|---|
| atlas image | `Renderer::loadAtlas(path, …)`          | `Renderer::loadAtlasFromMemory(bytes, …)` |
| map PNG      | `loadMapPng(path, …)`                   | `loadMapPngFromMemory(bytes, …)` |
| audio        | `AudioLibrary::registerAudio(path, …)`  | `AudioLibrary::uploadAudio(bytes, …)` |
| VM routine   | `Vm::registerRoutine(path, …)`          | `Vm::uploadRoutine(bytes, …)` |

- The **path door** takes a compile-time literal logical path and an optional `AssetPolicy`. The build
  sees the literal, so it can bake or copy the file for you; this is the door you use for assets that are
  part of the project.
- The **bytes door** takes a ready byte span. It carries **no** policy — you already have the bytes, so
  nothing is baked or copied. It is the escape hatch for a resource whose path you only know at runtime
  (read/assemble/decode it yourself, then hand over the bytes).

## Choosing the policy

On a path door the policy is an optional argument:

```cpp
// Embed — explicitly:
auto font = renderer.loadAtlas("game/assets/font.png", AssetDimensions::GameBoy8x8,
                               ContentKind::Tileset, ReadOrder::LeftRightThenDown,
                               /*count=*/0, /*transparentIndex=*/-1, /*framesPerAnimation=*/0,
                               AssetPolicy::Embed);

// LoadFromPath — explicitly:
auto mods = renderer.loadAtlas("game/assets/skin.png", AssetDimensions::GameBoy8x8,
                               ContentKind::Tileset, ReadOrder::LeftRightThenDown,
                               /*count=*/0, /*transparentIndex=*/-1, /*framesPerAnimation=*/0,
                               AssetPolicy::LoadFromPath);

// No policy argument — the per-type default applies:
auto map  = loadMapPng("game/assets/world.png");                 // loadMapPng     default → Embed
auto menu = renderer.loadAtlas("game/assets/menu.png",           // loadAtlas      default → LoadFromPath
                               AssetDimensions::GameBoy8x8, ContentKind::Tileset);
auto blip = AudioLibrary::instance().registerAudio("game/audio/blip.asm",   // chiptune .asm  → Embed
                                                   AudioType::Sfx, Isa::Sm83);
```

The effective policy is resolved by precedence (`resolveAssetPolicy`):

1. **The per-call argument**, if given.
2. Otherwise the **per-type default**, which follows what the input *is*:

   | Door | Input | Per-type default | Why |
   |---|---|---|---|
   | `loadAtlas`       | atlas image                            | `LoadFromPath` | atlases are the most likely copyright surface |
   | `loadMapPng`      | map PNG                                 | `Embed`        | bespoke build-time index data, not a shippable asset |
   | `registerAudio`   | chiptune (`.asm`)                       | `Embed`        | a driver is a few hundred bytes — assembled to bytecode at build, only bytecode ships |
   | `registerAudio`   | PCM (`.wav` / `.ogg` / `.flac` / `.mp3`) | `LoadFromPath` | a multi-MB track streams from disk; bytes are never baked unless you ask |
   | `registerRoutine` | VM routine (`.asm`)                     | `Embed`        | assembled to bytecode at build, only bytecode ships |

For audio, the kind (chiptune vs PCM) is inferred once from the file extension and frozen into the entry,
which is what selects the per-type default. The same precedence is evaluated identically at build time (to
decide what to bake vs. copy) and at runtime (to decide where to read from), so the two never disagree.
The only way to deviate from a per-type default is the explicit per-call argument — visible right at the
call site, never changed from a distance.

## Paths are logical; the engine resolves them

A path door's path is a **logical, project-root-relative** path (e.g. `"game/assets/world.png"`, or
`"game/audio/blip.asm"`) — and it is a *string literal* (a runtime-computed path can't be baked, so the
type rejects it at compile time). The **same string** addresses the input in both contexts:

- At build time the path is resolved against the project source root to read the file (to bake it, or to
  copy it).
- At runtime an Embed input is found by that logical key in the binary; a LoadFromPath one is resolved
  against the **asset root** and read from disk.

There is a single asset root for every family — images, audio, and routines all resolve LoadFromPath files
the same way; there is **no** separate audio or routine root. You never build a base-path string yourself.
The root is `EngineConfig::assetRoot` — by default the executable's own directory, which is exactly where
the build copies LoadFromPath files (preserving their logical path), so build and runtime agree with zero
configuration. Point it elsewhere for a game that ships its files in a subfolder or extracts them at
install time:

```cpp
EngineConfig config{ .assetRoot = "data" };   // LoadFromPath files resolve under <exe>/data/...
```

`setActive` resolves `assetRoot` to an absolute path once (against the executable directory) and the
loaders use it internally — the one place the base directory is consulted.

### A LoadFromPath routine is assembled, never baked

A LoadFromPath `.asm` (audio driver or VM routine) ships beside the binary, is read at registration, and
is assembled in-process **once at startup** by the engine's own assembler (the Game Boy family → SM83, no
external toolchain). Its bytes are **never baked into the executable** — which is exactly what makes
LoadFromPath the right policy for a copyright-derived routine that must not ship inside the binary. An
Embed `.asm`, by contrast, is assembled to bytecode at *build* time, and only that bytecode ships.

### Loading a file whose path you only know at runtime

The path doors take a **literal** logical path on purpose — that is what the build can see to bake or copy.
A non-literal path (a `std::string`, a `constexpr` constant, a name from a table, a user-picked file) is a
**compile error**, not a silent miss. The literal-only restriction is the same one `assetPath()` carries,
so neither a path door nor `assetPath()` can resolve a runtime-chosen name. To ingest such a file, locate
it yourself — `assetRoot()` is the public runtime base — read its bytes, and use the family's **bytes
door** (`loadAtlasFromMemory` / `loadMapPngFromMemory` / `uploadAudio` / `uploadRoutine`). Those take bytes
rather than a path, are never baked or copied (you ship the file), and are the explicit "this input is mine
to manage" escape hatch:

```cpp
// `name` is a runtime value, so it goes through the bytes door, not a literal path door:
auto sheet = renderer.loadAtlasFromMemory(readFile(assetRoot() / name), AssetDimensions::GameBoy8x8,
                                          ContentKind::Tileset);
```

The atlas slicer's exhaustive demo (`atlas_load_demo`) reads a runtime table of filenames this way.

## What the build does, automatically

The build scans each engine-linking target's sources for `loadAtlas` / `loadMapPng` / `registerAudio` /
`registerRoutine` calls, resolves each input's policy by the precedence above, and:

- **Embed** → bakes the bytes into the binary (an atlas/PNG's raw bytes, or a `.asm`'s assembled bytecode),
  decoded or run at runtime from memory.
- **LoadFromPath** → copies the file next to the binary at its logical path.

This is applied to every target that links `retropp::engine` — you do not call anything in CMake. An input
whose policy resolves to Embed but whose file isn't found at the project path falls back to a runtime disk
read rather than failing the build, and a LoadFromPath input is never baked — the safe direction for
copyright. The bytes doors are not scanned: you supplied the bytes, so there is nothing to bake or copy.

## Worked example

`examples/asset_embed_demo.cpp` renders one scene from three assets, one per outcome:

| Asset | Call | Policy | Delivery |
|---|---|---|---|
| map  | `loadMapPng("…map.png")`                       | Embed (default) | baked into the binary |
| font | `loadAtlas("…font.png", …, AssetPolicy::Embed)` | Embed (explicit) | baked into the binary |
| menu | `loadAtlas("…menu.png", …)`                    | LoadFromPath (default) | copied beside the binary |

After building, the demo's directory contains only the binary and the one menu PNG — the map and font live
inside the executable. Every call passes the same kind of bare logical path; the policy argument is the
only thing that differs. Audio and VM routines behave identically: a `registerAudio`/`registerRoutine`
call with the same policy argument bakes or ships the `.asm` the same way.

## Status

PCM audio is recognized as a kind for delivery purposes (extension detection, the `LoadFromPath` default,
the bytes door), so its policy already resolves correctly; the sample-mixer playback path itself is a
forthcoming seam. Chiptune and VM-routine delivery are fully realized today.

## Related

- [images-and-transparency.md](images-and-transparency.md) — `loadAtlas` / `loadPng` and atlas slicing.
- [tilemaps.md](tilemaps.md) — the map-PNG → `TileCatalog` → tile-layer pipeline.
- [vm-and-routines.md](vm-and-routines.md) — the `registerRoutine` / `uploadRoutine` API and the VM.
- [audio.md](audio.md) — registering and cueing audio (`registerAudio`, `AudioLibrary`, `AudioSystem`).
