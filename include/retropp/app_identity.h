#pragma once

#include <string>

namespace retropp {

// The application's identity — who this program is to the host platform. Declared once, on
// EngineConfig::identity, and projected by whatever subsystem needs to present the program
// to the outside world; SaveStore is the first consumer (the identity names the per-user
// save directory). Every member is a public identity FACT; extension is additive.
//
// Secrets are never members here: anything this struct holds at runtime ships as bytes
// inside the binary, so signing credentials, keys, and other deploy-time material live in
// deploy-side configuration that REFERENCES this identity — never inside it.
//
// Neither field has a default or a fallback — an identity the developer didn't choose is
// wrong for anything that persists (every unconfigured program would share one name, and
// their saves would collide), so consumers refuse an unset identity loudly instead. Set
// both fields once and keep them stable forever: a changed identity is a different save
// directory, and the players' documents stay stranded under the old one.
struct AppIdentity {
    std::string organization;  // no default — consumers refuse an empty field
    std::string application;   // no default — consumers refuse an empty field
};

}  // namespace retropp
