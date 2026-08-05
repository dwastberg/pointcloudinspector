#pragma once

#include "foundation/Vec3d.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace pci {

struct VectorLayerData;

struct VectorVertex2f {
    float x = 0.0F;
    float y = 0.0F;
    bool operator==(const VectorVertex2f &) const = default;
};

struct VectorSegment2f {
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
    bool operator==(const VectorSegment2f &) const = default;
};

enum class VectorGeometryKind : std::uint8_t {
    Point = 0,
    Line = 1,
    Polygon = 2,
};

inline constexpr std::size_t maximumVerticesPerFillBatch = 65'536;
inline constexpr double maximumOriginRelativeMetres = 200'000.0;

struct VectorFillBatch {
    std::vector<VectorVertex2f> vertices;
    std::vector<std::uint16_t> indices;
};

struct VectorGeometrySummary {
    std::uint64_t pointParts = 0;
    std::uint64_t lineParts = 0;
    std::uint64_t polygonParts = 0;
    std::uint64_t skippedFeatures = 0;
    std::uint64_t skippedParts = 0;
    std::uint64_t unfilledPolygons = 0;
    bool sourceHadZ = false;

    [[nodiscard]] VectorGeometryKind dominantKind() const noexcept;
};

struct VectorImportLimits {
    std::uint64_t maximumSourceFeatures = 2'000'000;
    std::uint64_t maximumInputPolygonVertices = 16'000'000;
    std::uint64_t maximumEmittedFillVertices = 16'000'000;
    std::uint64_t maximumFillIndices = 48'000'000;
    std::uint64_t maximumSegments = 8'000'000;
    std::uint64_t maximumMarkers = 8'000'000;
    std::uint64_t maximumRetainedBytes = std::uint64_t{256} * 1024 * 1024;
    std::uint64_t maximumApplicationWorkingBytes =
        std::uint64_t{256} * 1024 * 1024;
    std::uint64_t maximumCurveControlPoints = 100'000;
    std::uint64_t maximumLinearizedCurvePoints = 1'000'000;
    double curveMaximumAngleStepDegrees = 4.0;
};

class VectorImportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class VectorImportLimitExceeded final : public VectorImportError {
public:
    VectorImportLimitExceeded(const char *limit, std::uint64_t value);
};

class VectorImportCancelled final : public VectorImportError {
public:
    using VectorImportError::VectorImportError;
};

using VectorRing = std::vector<std::array<double, 2>>;

[[nodiscard]] Vec3d vectorLayerOrigin(double x, double y) noexcept;
[[nodiscard]] bool
validVectorImportLimits(const VectorImportLimits &limits) noexcept;

class VectorGeometryBuilder {
public:
    VectorGeometryBuilder(Vec3d origin, VectorImportLimits limits);

    [[nodiscard]] bool addPoint(double x, double y);
    [[nodiscard]] bool
    addLineString(std::span<const std::array<double, 2>> vertices, bool closed);
    [[nodiscard]] bool addPolygon(std::span<const VectorRing> rings);
    void markSkippedFeature() noexcept;
    void markSkippedPart() noexcept;
    void markSourceHadZ() noexcept;
    [[nodiscard]] VectorLayerData build() &&;
    [[nodiscard]] const VectorImportLimits &limits() const noexcept
    {
        return limits_;
    }

private:
    struct RemapEntry {
        std::uint32_t generation = 0;
        std::uint16_t localIndex = 0;
        std::uint16_t padding = 0;
    };
    static_assert(sizeof(RemapEntry) == 8);

    [[nodiscard]] bool validCoordinate(double x, double y) const noexcept;
    void appendSegment(const VectorVertex2f &from, const VectorVertex2f &to);
    void appendMarker(const VectorVertex2f &point);
    void appendFillTriangle(const std::vector<VectorVertex2f> &vertices,
                            std::array<std::uint32_t, 3> triangle,
                            std::vector<RemapEntry> &remap,
                            std::uint32_t &generation);
    void extendBounds(double x, double y);
    void ensureLimit(const char *name,
                     std::uint64_t current,
                     std::uint64_t addition,
                     std::uint64_t limit) const;
    void ensureRetained(std::uint64_t addition) const;
    void ensureWorking(std::uint64_t transientBytes) const;
    [[nodiscard]] std::uint64_t retainedBytes() const noexcept;

    Vec3d origin_;
    VectorImportLimits limits_;
    std::vector<VectorFillBatch> fillBatches_;
    std::vector<VectorSegment2f> segments_;
    std::vector<VectorVertex2f> markers_;
    VectorGeometrySummary summary_;
    std::uint64_t inputPolygonVertices_ = 0;
    std::uint64_t emittedFillVertices_ = 0;
    std::uint64_t fillIndices_ = 0;
    bool hasBounds_ = false;
    std::array<double, 2> minimum_{};
    std::array<double, 2> maximum_{};
};

} // namespace pci
