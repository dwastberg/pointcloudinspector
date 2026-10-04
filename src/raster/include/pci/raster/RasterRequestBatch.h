#pragma once

#include <pci/foundation/Generation.h>
#include <pci/raster/RasterTileSource.h>

#include <cstdint>
#include <memory>
#include <span>

namespace pci {

// One source's ordered contribution to frame-global raster reconciliation.
// The tile-key span is borrowed only for the duration of reconcile(); queued
// requests copy every value they retain. The source pointer deliberately
// lives here, beside its strong binding identity, rather than in a document
// layer or planner object.
struct RasterRequestBatch {
    RasterSourceId sourceId;
    BindingGeneration bindingGeneration;
    std::uint64_t renderGeneration = 0;
    RasterTileSourcePtr source;
    std::shared_ptr<const RasterDecodeParameters> decode;
    RasterTilePayloadProfile profile = RasterTilePayloadProfile::ColorOnly;
    std::span<const RasterTileKey> orderedRequests;
};

} // namespace pci
