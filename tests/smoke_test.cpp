#include <gtest/gtest.h>

#include "gbcpp/version.h"
#include "gbcpp/testkit/testkit.h"

// Engine smoke test (acceptance item 2): asserts a real engine value. Genuinely
// failable — blanking kVersion in src/version.cpp turns this red.
TEST(EngineSmoke, VersionIsNonEmpty) {
    EXPECT_FALSE(gbcpp::version().empty());
}

// Link-surface fixture (acceptance item 3): this TU links gbcpp::testkit via its
// namespaced alias exactly as a mode-3 consumer would, proving the test-tooling
// target resolves and links before its real body exists (ENG-7).
TEST(EngineSmoke, TestkitLinkSurfaceResolves) {
    EXPECT_FALSE(gbcpp::testkit::testkit_id().empty());
}
