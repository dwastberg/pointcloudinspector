#include "vector/VectorLayerData.h"

#include "foundation/CheckedArithmetic.h"

#include <sstream>

namespace pci {
bool VectorLayerData::empty() const noexcept
{
    return fillBatches.empty() && segments.empty() && markers.empty();
}

std::uint64_t VectorLayerData::byteSize() const noexcept
{
    std::uint64_t result = sizeof(VectorLayerData);
    for (const VectorFillBatch &batch : fillBatches) {
        result = saturatingAdd(
            result, static_cast<std::uint64_t>(sizeof(VectorFillBatch)));
        result =
            saturatingAdd(result,
                          saturatingMultiply<std::uint64_t>(
                              batch.vertices.size(), sizeof(VectorVertex2f)));
        result =
            saturatingAdd(result,
                          saturatingMultiply<std::uint64_t>(
                              batch.indices.size(), sizeof(std::uint16_t)));
    }
    result = saturatingAdd(result,
                           saturatingMultiply<std::uint64_t>(
                               segments.size(), sizeof(VectorSegment2f)));
    result = saturatingAdd(result,
                           saturatingMultiply<std::uint64_t>(
                               markers.size(), sizeof(VectorVertex2f)));
    result = saturatingAdd(
        result, static_cast<std::uint64_t>(sourcePath.native().size()));
    result =
        saturatingAdd(result, static_cast<std::uint64_t>(sourceDriver.size()));
    result =
        saturatingAdd(result, static_cast<std::uint64_t>(sublayerName.size()));
    return saturatingAdd(
        result, static_cast<std::uint64_t>(spatialReferenceWkt.size()));
}

std::uint64_t VectorLayerData::triangleCount() const noexcept
{
    std::uint64_t result = 0;
    for (const VectorFillBatch &batch : fillBatches) {
        result = saturatingAdd(
            result, static_cast<std::uint64_t>(batch.indices.size() / 3U));
    }
    return result;
}

std::string VectorLayerData::geometryDescription() const
{
    std::ostringstream result;
    result << summary.pointParts << " point parts, " << summary.lineParts
           << " line parts, " << summary.polygonParts << " polygon parts";
    if (summary.unfilledPolygons != 0) {
        result << "; " << summary.unfilledPolygons << " outline-only polygons";
    }
    return result.str();
}

} // namespace pci
