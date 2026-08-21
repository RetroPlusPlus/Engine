#pragma once

#include <string_view>

namespace retropp::testkit {

// Placeholder identity for the test-tooling target. The full SameBoy-core
// harness wrapper and scenario base class land later; this exists so the
// retropp::testkit link surface is real from the start. Never empty.
[[nodiscard]] std::string_view testkit_id() noexcept;

}  // namespace retropp::testkit
