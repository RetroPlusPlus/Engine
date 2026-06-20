# Assets — embed vs. load-from-path

Every ingestible asset — an atlas image (`loadAtlas`) or a map PNG (`loadMapPng`) — is delivered one of
two ways, and **the choice lives in your code, not in the build system**:

- **Embed** — the asset's bytes are baked into the executable at build time and decoded from memory at
  runtime. The source file never ships. Use it for self-contained binaries, art you don't want altered,
  and build-time-only design inputs (a tilemap's index PNG).
- **LoadFromPath** — the asset ships beside the binary (or is extracted there) and is read from disk at
  runtime. Use it for moddable assets and — importantly — for any asset that must **never** be baked into
  a shipped binary (e.g. copyright-derived art a port populates at runtime from the user's own ROM).

You write the `loadAtlas` / `loadMapPng` call with a policy; the build does the rest automatically — it
**bakes** the Embed assets into the binary and **copies** the LoadFromPath assets next to it. There is no
build rule to write, no copy step to add, and no path to construct.

## Choosing the policy

The policy is an optional last argument on the loaders:

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
auto map = loadMapPng("game/assets/world.png");          // loadMapPng default → Embed
auto menu = renderer.loadAtlas("game/assets/menu.png", AssetDimensions::GameBoy8x8,
                               ContentKind::Tileset);     // loadAtlas default → LoadFromPath
```

The effective policy for an asset is resolved by precedence (`resolveAssetPolicy`):

1. **The per-call argument**, if given.
2. Otherwise the **per-type default**: `loadMapPng` → `Embed` (a map PNG is bespoke build-time index data),
   `loadAtlas` → `LoadFromPath` (atlases are the most likely copyright surface).

The same precedence is evaluated identically at build time (to decide what to bake vs. copy) and at
runtime (to decide where to read from), so the two never disagree. The only way to deviate from a
loader's per-type default is the explicit per-call argument — visible right at the call site, never
changed from a distance.

## Paths are logical; the engine resolves them

A loader's path is a **logical, project-root-relative** path (e.g. `"game/assets/world.png"`) — and it is
a *string literal* (a runtime-computed path can't be baked, so the type rejects it at compile time). The
**same string** addresses the asset in both contexts:

- At build time the path is resolved against the project source root to read the file (to bake it, or to
  copy it).
- At runtime an Embed asset is found by that logical key in the binary; a LoadFromPath asset is resolved
  against the **asset root** and read from disk.

You never build a base-path string yourself. The asset root is `EngineConfig::assetRoot` — by default the
executable's own directory, which is exactly where the build copies LoadFromPath assets (preserving their
logical path), so build and runtime agree with zero configuration. Point it elsewhere for a game that
ships assets in a subfolder or extracts them at install time:

```cpp
EngineConfig config{ .assetRoot = "data" };   // LoadFromPath assets resolve under <exe>/data/...
```

`setActive` resolves `assetRoot` to an absolute path once (against the executable directory) and the
loaders use it internally — the one place the base directory is consulted.

### Loading a file whose path you only know at runtime

The loaders take a **literal** logical path on purpose — that is what the build can see to bake or copy.
A non-literal path (a `std::string`, a `constexpr` constant, a name from a table, a user-picked file) is a
**compile error**, not a silent miss. To load such a file, read its bytes yourself and use
`loadAtlasFromMemory` / `loadMapPngFromMemory` — those take bytes rather than a path, are never baked or
copied (you ship the file), and are the explicit "this asset is mine to manage" escape hatch. The atlas
slicer's exhaustive demo (`atlas_load_demo`) does exactly this for its table of filenames:

```cpp
auto sheet = renderer.loadAtlasFromMemory(readFile(assetPath(name)), AssetDimensions::GameBoy8x8,
                                          ContentKind::Tileset);
```

## What the build does, automatically

The build scans each engine-linking target's sources for `loadAtlas` / `loadMapPng` calls, resolves each
asset's policy by the precedence above, and:

- **Embed** → reads the file and bakes its bytes into the binary (decoded at runtime from memory).
- **LoadFromPath** → copies the file next to the binary at its logical path.

This is applied to every target that links `retropp::engine` — you do not call anything in CMake. An
asset whose policy resolves to Embed but whose file isn't found at the project path falls back to a
runtime disk read rather than failing the build, and a LoadFromPath asset is never baked — the safe
direction for copyright.

## Worked example

`examples/asset_embed_demo.cpp` renders one scene from three assets, one per outcome:

| Asset | Call | Policy | Delivery |
|---|---|---|---|
| map  | `loadMapPng("…map.png")`                       | Embed (default) | baked into the binary |
| font | `loadAtlas("…font.png", …, AssetPolicy::Embed)` | Embed (explicit) | baked into the binary |
| menu | `loadAtlas("…menu.png", …)`                    | LoadFromPath (default) | copied beside the binary |

After building, the demo's directory contains only the binary and the one menu PNG — the map and font
live inside the executable. Every call passes the same kind of bare logical path; the policy argument is
the only thing that differs.

## Related

- [images-and-transparency.md](images-and-transparency.md) — `loadAtlas` / `loadPng` and atlas slicing.
- [tilemaps.md](tilemaps.md) — the map-PNG → `TileCatalog` → tile-layer pipeline.
