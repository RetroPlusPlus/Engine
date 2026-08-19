// DataLibrary implementation: a catalog of byte buffers, resolved on demand and held for the program.
#include "retropp/data_library.h"

#include <fstream>
#include <ios>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "retropp/asset_registry.h"  // assetRoot, detail::findEmbeddedAsset, detail::warnEmbedNotBaked

namespace retropp {

DataLibrary& DataLibrary::instance() {
    // Function-local static: constructed on first use (unreferenced ⇒ not linked), destroyed at program
    // exit, thread-safe initialization. The single instance the header guarantees.
    static DataLibrary library;
    return library;
}

DataId DataLibrary::uploadData(std::span<const std::uint8_t> bytes) {
    Entry entry{
        .policy = {},  // raw bytes carry no embed/load policy — you brought the bytes
        .path   = {},
        .owned  = std::vector<std::uint8_t>(bytes.begin(), bytes.end()),  // owned copy
    };
    // Point the span at this entry's own buffer before the entry moves into the vector. The buffer is on
    // the heap, so the move carries the pointer with it and a later reallocation of entries_ leaves it
    // where it is — which is what makes the span data() returns stable for the life of the program.
    entry.resolved = entry.owned;
    entry.ready    = true;
    entries_.push_back(std::move(entry));
    return static_cast<DataId>(entries_.size() - 1);
}

DataId DataLibrary::registerData(LiteralPath resourcePath, std::optional<AssetPolicy> policy) {
    entries_.push_back(Entry{
        .policy = policy,
        .path   = std::string(resourcePath.view()),
    });
    return static_cast<DataId>(entries_.size() - 1);
}

std::span<const std::uint8_t> DataLibrary::data(DataId id) {
    const auto index = static_cast<std::size_t>(id);
    if (index >= entries_.size()) {
        throw std::out_of_range("DataLibrary::data: no data is registered for this DataId");
    }
    Entry& entry = entries_[index];
    if (entry.ready) {
        return entry.resolved;
    }

    // Embed: the build baked this path's bytes into the binary, keyed by the logical path. An empty
    // result means the build baked nothing for it — warn once naming the path, then read from disk, which
    // is the fallback every other family takes and the diagnosis a shipped binary needs.
    if (resolveAssetPolicy(entry.policy, AssetPolicy::LoadFromPath) == AssetPolicy::Embed) {
        if (const std::span<const std::uint8_t> baked = detail::findEmbeddedAsset(entry.path);
            !baked.empty()) {
            entry.resolved = baked;
            entry.ready    = true;
            return entry.resolved;
        }
        detail::warnEmbedNotBaked("asset", entry.path);
    }

    const std::filesystem::path full = assetRoot() / std::filesystem::path(entry.path);
    std::ifstream in{full, std::ios::binary};
    if (!in) {
        throw std::runtime_error("DataLibrary::data: cannot open data file: " + full.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string contents = ss.str();
    entry.owned.assign(contents.begin(), contents.end());
    entry.resolved = entry.owned;
    entry.ready    = true;
    return entry.resolved;
}

}  // namespace retropp
