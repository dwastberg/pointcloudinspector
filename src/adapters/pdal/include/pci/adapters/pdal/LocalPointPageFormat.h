#pragma once

#include <pci/adapters/storage/SecureStorage.h>
#include <pci/pointcloud/BlockPartitioner.h>
#include <pci/pointcloud/PointCloudMetadata.h>
#include <pci/pointcloud/PointCloudNode.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace pci {

inline constexpr std::uint32_t localPointPageFormatVersion = 2;
inline constexpr std::uint32_t localPointPageSchemaVersion = 1;
inline constexpr std::uint32_t localPointPageBuildRevision = 5;
inline constexpr std::size_t localPointDiskBytes = 34;

struct LocalPointPageStoreOptions {
    LocalPageCacheContextPtr cache;
    std::uint32_t pointsPerLeaf = 32'768;
    std::uint32_t rootPreviewPoints = 16'384;
    std::uint64_t sortMemoryBytes = std::uint64_t{64} * 1024 * 1024;
    std::uint64_t diskCacheBytes = std::uint64_t{20} * 1024 * 1024 * 1024;

    bool operator==(const LocalPointPageStoreOptions &) const = default;
};

using LocalPointSourceFingerprint = std::array<std::uint8_t, 32>;
using LocalPointPayloadDigest = std::array<std::uint8_t, 32>;

struct LocalPointPageRecord {
    PointCloudNodeId id;
    Bounds3d tightBounds;
    std::uint64_t sourcePointCount = 0;
    std::uint64_t pointCount = 0;
    std::uint64_t payloadOffset = 0;
    std::uint64_t payloadBytes = 0;
    LocalPointPayloadDigest payloadDigest{};
};

struct LocalPointScalarRanges {
    std::uint16_t intensityMinimum = 0;
    std::uint16_t intensityMaximum = 0;
    std::uint8_t classificationMinimum = 0;
    std::uint8_t classificationMaximum = 0;
    std::uint8_t returnNumberMinimum = 0;
    std::uint8_t returnNumberMaximum = 0;
};

struct LocalPointPageManifest {
    LocalPointSourceFingerprint fingerprint{};
    PointCloudMetadata metadata;
    std::filesystem::path canonicalSourcePath;
    std::uint64_t sourceFileBytes = 0;
    std::uint64_t sourceModificationTime = 0;
    std::uint64_t payloadFileBytes = 0;
    std::uint8_t maximumLevel = 0;
    std::uint32_t pointsPerLeaf = 0;
    std::uint32_t rootPreviewPoints = 0;
    LocalPointScalarRanges scalarRanges;
    std::vector<LocalPointPageRecord> pages;
};

[[nodiscard]] LocalPointSourceFingerprint
fingerprintLocalPointSource(const PointCloudMetadata &metadata,
                            std::uint64_t sourceFileBytes,
                            std::uint64_t sourceModificationTime,
                            const LocalPointPageStoreOptions &options);
[[nodiscard]] std::string
localPointFingerprintHex(const LocalPointSourceFingerprint &fingerprint);
[[nodiscard]] std::uint64_t localPointCurrentProcessId() noexcept;
[[nodiscard]] bool localPointProcessAlive(std::uint64_t processId) noexcept;

[[nodiscard]] LocalPointPayloadDigest
localPointPayloadDigest(std::span<const std::byte> bytes);
[[nodiscard]] std::array<std::byte, localPointDiskBytes>
encodeLocalPoint(const PointSample &point) noexcept;
[[nodiscard]] PointSample
decodeLocalPoint(std::span<const std::byte, localPointDiskBytes> bytes);

void writeLocalPointManifest(
    const std::filesystem::path &path,
    const LocalPointPageManifest &manifest,
    const ManifestAuthenticationKey &authenticationKey);
[[nodiscard]] LocalPointPageManifest
readLocalPointManifest(const std::filesystem::path &path,
                       const LocalPointSourceFingerprint &expectedFingerprint,
                       const ManifestAuthenticationKey &authenticationKey);

} // namespace pci
