#pragma once

#include "import/local/LocalPointPageFormat.h"
#include "scene/PointCloudDataSource.h"

#include <filesystem>
#include <memory>

namespace pci {

class LocalPointIndexBuilder;
struct LocalPointPageSourceState;

class LocalPointPageSource final : public PointCloudDataSource {
public:
    [[nodiscard]] PointCloudNode rootNode() const override;
    [[nodiscard]] PointCloudNode node(PointCloudNodeId id) const override;
    [[nodiscard]] PointCloudNodePayloadPtr
    loadNode(PointCloudNodeId id, std::stop_token stopToken) const override;
    [[nodiscard]] PointCloudDataSourceMetrics metrics() const override;
    [[nodiscard]] PointCloudStorageMetrics storageMetrics() const override;
    [[nodiscard]] PointCloudScalarRanges scalarRanges() const override;
    [[nodiscard]] std::optional<PointCloudFullDetailInfo>
    fullDetailInfo() const override;
    [[nodiscard]] std::optional<std::vector<PointCloudStoredNode>>
    storedNodeIndex() const override;

    [[nodiscard]] std::uint8_t maximumLevel() const noexcept;
    [[nodiscard]] std::filesystem::path storeDirectory() const;
    [[nodiscard]] bool committed() const noexcept;

    // Builder-facing lifecycle. These remain public so the format's bounded
    // streaming helpers can publish pages without exposing mutable scene APIs.
    LocalPointPageSource(std::shared_ptr<LocalPointPageSourceState> state,
                         std::uint64_t maximumPoints);

    [[nodiscard]] static std::shared_ptr<LocalPointPageSource>
    createBuilding(PointCloudMetadata metadata,
                   std::filesystem::path payloadPath,
                   std::filesystem::path storeDirectory,
                   LocalPointSourceFingerprint fingerprint,
                   std::uint8_t maximumLevel,
                   std::uint32_t pointsPerLeaf,
                   std::uint64_t maximumPoints);
    [[nodiscard]] static std::shared_ptr<LocalPointPageSource>
    openCommitted(const std::filesystem::path &storeDirectory,
                  const LocalPointSourceFingerprint &fingerprint,
                  std::uint64_t maximumPoints);

    void publish(LocalPointPageRecord record);
    void setScalarRanges(PointCloudScalarRanges ranges);
    void finishCommit(std::filesystem::path finalStoreDirectory);
    void fail(std::string message) noexcept;

private:
    friend class LocalPointIndexBuilder;

    std::shared_ptr<LocalPointPageSourceState> state_;
    std::uint8_t maximumLevel_ = 0;
};

using LocalPointPageSourcePtr = std::shared_ptr<LocalPointPageSource>;

} // namespace pci
