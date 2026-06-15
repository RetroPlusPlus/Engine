#include "retropp/version.h"

namespace retropp {

namespace {
// Bumped at each engine release; "-dev" denotes an unreleased working tree.
constexpr std::string_view kVersion = "0.1.0-dev";
}  // namespace

std::string_view version() noexcept {
    return kVersion;
}

}  // namespace retropp
