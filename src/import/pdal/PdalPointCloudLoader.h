#pragma once

#include "import/PointCloudImport.h"

namespace pci {

class PdalPointCloudLoader final : public PointCloudLoader {
public:
    [[nodiscard]] PointCloudImportPreflight
    inspect(const PointCloudLoadOptions &options) const;
    [[nodiscard]] PointCloudImportPreflight
    inspect(const PointCloudLoadRequest &request,
            std::stop_token stopToken = {}) const;
    [[nodiscard]] PointCloudImportPreflight
    inspect(const PointCloudLoadOptions &options,
            std::uint64_t decodedByteBudget,
            std::stop_token stopToken) const override;
    [[nodiscard]] PointCloudScenePtr
    load(const PointCloudLoadOptions &options,
         const PointCloudLoadResources &resources = {},
         const PointCloudLoadContext &context = {}) const;
    [[nodiscard]] PointCloudScenePtr
    load(const PointCloudLoadRequest &request,
         const PointCloudLoadContext &context = {}) const;
    [[nodiscard]] PointCloudScenePtr
    load(const PointCloudLoadOptions &options,
         const PointCloudLoadResources &resources,
         const PointCloudImportPreflight &preflight,
         const PointCloudLoadContext &context) const override;
};

} // namespace pci
