#pragma once

#include <string_view>

namespace retropp {

// The platform's semantic version string (e.g. "0.1.0-dev"). Never empty.
[[nodiscard]] std::string_view version() noexcept;

}  // namespace retropp
