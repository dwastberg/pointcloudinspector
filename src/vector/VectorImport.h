#pragma once

#include "foundation/Bounds3d.h"
#include "vector/VectorLayerData.h"

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace pci {

struct VectorImportProgress {
    std::uint64_t processed = 0;
    std::uint64_t total = 0;
};

struct VectorSublayerKey {
    int index = -1;
    std::string name;
    bool operator==(const VectorSublayerKey &) const = default;
};

struct VectorSublayerInfo {
    VectorSublayerKey key;
    std::string geometryTypeLabel;
    std::string spatialReferenceWkt;
    std::optional<VectorGeometryKind> kind;
    std::int64_t featureCount = -1;
    bool hasZ = false;
    std::optional<Bounds3d> extent;
};

struct VectorImportPreflight {
    std::filesystem::path sourcePath;
    std::string driverName;
    std::vector<VectorSublayerInfo> sublayers;
};

struct VectorImportRequest {
    std::filesystem::path sourcePath;
    std::vector<VectorSublayerKey> sublayers;
    std::optional<std::array<double, 2>> origin;
    VectorImportLimits limits;
    std::string targetSpatialReferenceWkt;
    std::optional<Bounds3d> targetExtent;
    std::stop_token stopToken;
    std::function<void(VectorImportProgress)> progress;
};

struct VectorSublayerFailure {
    VectorSublayerKey key;
    std::string message;
    bool operator==(const VectorSublayerFailure &) const = default;
};

struct VectorLoadSummary {
    std::vector<VectorSublayerKey> selected;
    std::vector<VectorSublayerKey> successful;
    std::vector<VectorSublayerFailure> failed;

    [[nodiscard]] bool allSucceeded() const noexcept
    {
        return !selected.empty() && successful.size() == selected.size();
    }
    [[nodiscard]] bool partialSuccess() const noexcept
    {
        return !successful.empty() && !failed.empty();
    }
};

class VectorLoader {
public:
    virtual ~VectorLoader() = default;
    [[nodiscard]] virtual VectorImportPreflight
    inspect(const VectorImportRequest &request) const = 0;
    [[nodiscard]] virtual std::array<double, 2>
    probeOrigin(const VectorImportRequest &request,
                std::span<const VectorSublayerKey> sublayers) const = 0;
    [[nodiscard]] virtual VectorLayerDataPtr
    loadSublayer(const VectorImportRequest &request,
                 VectorSublayerKey sublayer) const = 0;
};

} // namespace pci
