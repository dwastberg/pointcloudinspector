#pragma once

#include <pci/adapters/pdal/LocalPointPageSource.h>
#include <pci/operations/PointCloudImport.h>

#include <functional>

namespace pci {

struct LocalPointPageBuildResult {
    LocalPointPageSourcePtr source;
    PointCloudNodePayloadPtr rootPayload;
    std::filesystem::path storeDirectory;
    std::uint64_t sourcePointsScanned = 0;
    std::uint64_t payloadBytes = 0;
    std::uint64_t pageCount = 0;
    bool reused = false;
};

class LocalPointIndexBuilder final {
public:
    using RootReady = std::function<void(const LocalPointPageSourcePtr &,
                                         const PointCloudNodePayloadPtr &)>;

    [[nodiscard]] LocalPointPageBuildResult
    openOrBuild(const PointCloudImportPreflight &preflight,
                std::uint64_t maximumPoints,
                const LocalPointPageStoreOptions &options,
                std::stop_token stopToken = {},
                std::function<void(PointCloudImportProgress)> progress = {},
                RootReady rootReady = {}) const;
};

} // namespace pci
