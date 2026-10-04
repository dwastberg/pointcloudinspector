#pragma once

#include <pci/operations/RasterImport.h>

#include <atomic>
#include <cstdint>

namespace pci {

// Performs inspection, band selection, level intersection, and bounded range
// sampling. It never forces statistics and never reads a full-resolution
// payload, so a metadata job cannot become as expensive as loading the source.
class GdalRasterLoader final : public RasterLoader {
public:
    [[nodiscard]] RasterImportPreflight
    inspect(const RasterImportRequest &request) const override;

    // Pixel reads issued by range sampling. Bounded by the sample budget and
    // independent of source dimensions; asserting on it is what keeps a
    // regression that starts scanning the base image from merely looking slow.
    [[nodiscard]] std::uint64_t sampleReadCount() const noexcept
    {
        return sampleReadCount_.load(std::memory_order_relaxed);
    }

private:
    mutable std::atomic<std::uint64_t> sampleReadCount_{0};
};

} // namespace pci
