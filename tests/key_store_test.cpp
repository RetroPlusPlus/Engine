// KeyStore: a per-frame arena that keeps runtime-built reconciliation-key strings alive so a Sprite's
// ObjectKey (a non-owning view) stays valid for the frame that submits it. The identity is the string
// VALUE the game re-emits each frame; the store only owns the backing bytes until the next clear().

#include "retropp/draw_state.h"

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace retropp {
namespace {

TEST(KeyStore, InternReturnsAKeyEqualToTheInput) {
    KeyStore keys;
    const ObjectKey k = keys("enemy_" + std::to_string(7));
    EXPECT_EQ(std::string_view(k), "enemy_7");
    EXPECT_EQ(keys.size(), 1u);
}

TEST(KeyStore, EarlierKeysStayValidAsMoreAreInterned) {
    // The whole reason the store is deque-backed: interning more keys must NOT invalidate the views
    // already handed out (a vector would reallocate and dangle them). Intern many, then re-read the first.
    KeyStore keys;
    std::vector<ObjectKey> handed;
    handed.reserve(1000);
    for (int i = 0; i < 1000; ++i) handed.push_back(keys("obj_" + std::to_string(i)));
    for (int i = 0; i < 1000; ++i)
        EXPECT_EQ(std::string_view(handed[static_cast<std::size_t>(i)]), "obj_" + std::to_string(i));
    EXPECT_EQ(keys.size(), 1000u);
}

TEST(KeyStore, ClearEmptiesTheStoreForReuse) {
    KeyStore keys;
    static_cast<void>(keys("a"));
    static_cast<void>(keys("b"));
    EXPECT_EQ(keys.size(), 2u);
    keys.clear();
    EXPECT_EQ(keys.size(), 0u);
    // A fresh frame's key with the same value is a distinct object equal by value — the identity is the
    // string content the game re-emits, not the storage.
    const ObjectKey again = keys("a");
    EXPECT_EQ(std::string_view(again), "a");
    EXPECT_EQ(keys.size(), 1u);
}

TEST(KeyStore, DistinctInternsOfEqualValuesCompareEqual) {
    // Two interned copies of the same value are equal ObjectKeys — matching (in the interpolator) is by
    // value, so re-emitting the same name next frame reconciles to the same object.
    KeyStore keys;
    const ObjectKey a = keys(std::string("player"));
    const ObjectKey b = keys(std::string("player"));
    EXPECT_EQ(a, b);
}

}  // namespace
}  // namespace retropp
