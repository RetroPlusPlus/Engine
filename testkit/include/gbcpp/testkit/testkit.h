#pragma once

#include <string_view>

namespace gbcpp::testkit {

// Placeholder identity for the test-tooling target. The full SameBoy-core
// harness wrapper and scenario base class land at ENG-7; this exists so the
// gbcpp::testkit link surface is real from ENG-0. Never empty.
[[nodiscard]] std::string_view testkit_id() noexcept;

}  // namespace gbcpp::testkit
