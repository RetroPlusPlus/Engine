#include "retropp/testkit/testkit.h"

namespace retropp::testkit {

namespace {
constexpr std::string_view kId = "retropp-testkit-stub";
}  // namespace

std::string_view testkit_id() noexcept {
    return kId;
}

}  // namespace retropp::testkit
