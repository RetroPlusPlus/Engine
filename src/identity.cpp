#include "retropp/identity.h"

#include <atomic>

#include "retropp/draw_state.h"  // complete LayerId / SpriteId / RegionId

namespace retropp {

namespace {
// The single id faucet: a monotonic counter starting at 1, so the first id handed out is 1 and 0 stays
// reserved for "none". A function-local static (no global-init-order dependency); atomic so construction
// on the audio thread can mint without racing the main loop. Relaxed ordering — ids only need to be
// unique, not ordered against other memory.
std::uint32_t nextObjectId() noexcept {
    static std::atomic<std::uint32_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

LayerId  mintLayerId() noexcept  { return static_cast<LayerId>(nextObjectId()); }
SpriteId mintSpriteId() noexcept { return static_cast<SpriteId>(nextObjectId()); }
RegionId mintRegionId() noexcept { return static_cast<RegionId>(nextObjectId()); }

}  // namespace retropp
