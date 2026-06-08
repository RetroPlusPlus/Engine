#include "gbcpp/testkit/testkit.h"

namespace gbcpp::testkit {

namespace {
constexpr std::string_view kId = "gbcpp-testkit-stub";
}  // namespace

std::string_view testkit_id() noexcept {
    return kId;
}

}  // namespace gbcpp::testkit
