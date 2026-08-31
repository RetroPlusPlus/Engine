#pragma once

// The platform's developer-supplied identity type. A leaf header: it pulls nothing but the standard
// library, so any surface that needs a named identity reaches it without taking on that surface's
// vocabulary.

#include <string>
#include <string_view>

namespace retropp {

// A required reconciliation key: the stable, developer-supplied identity the renderer matches an object to
// its previous tick state by. It SURVIVES the frame being rebuilt each render — the game re-supplies the
// same key for the same object every frame (the immediate-mode model) — so per-object motion carries
// across ticks and the object eases between sim states. A key that does not survive the rebuild (e.g. a
// per-construction unique value) never matches its own prior frame, so interpolation could never engage;
// the developer key is the identity that does.
//
// It is REQUIRED: no default constructor, so omitting `.key` in a DrawLayer / Sprite / Region aggregate
// value-initializes the member, which calls the deleted constructor — a COMPILE ERROR, never a silent
// empty. The implicit conversions keep call sites reading like strings: `.key = "ball"` and
// `interp.interpolatedSpritePos(s.key, alpha)` (ObjectKey → string_view). The key names identity across
// frames — z alone orders depth, never the key.
//
// ObjectKey OWNS its bytes (a std::string), so a key assembled at runtime just works:
// `.key = "enemy_" + std::to_string(id)` moves that string in and the identity outlives the frame with no
// lifetime dance. Short reconciliation keys ("enemy_5") stay inside the string's small-buffer, off the heap.
//
// Named ObjectKey (not Key) so it never collides with a game's own "key" — keyboard keys, keypad keys —
// under `using namespace retropp`.
struct ObjectKey {
    std::string value;
    ObjectKey() = delete;
    ObjectKey(const char* v) : value(v) {}
    ObjectKey(std::string_view v) : value(v) {}
    ObjectKey(std::string v) noexcept : value(std::move(v)) {}
    [[nodiscard]] operator std::string_view() const noexcept { return value; }
    [[nodiscard]] bool operator==(const ObjectKey&) const noexcept = default;
};

}  // namespace retropp
