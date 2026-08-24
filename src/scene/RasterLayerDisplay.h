#pragma once

#include "scene/SceneDocument.h"

namespace pci {

// Combines immutable source defaults with the scene style for one render
// generation. The returned parameters remain valid for asynchronous reads.
[[nodiscard]] std::shared_ptr<const RasterDecodeParameters>
resolveRasterDecodeParameters(const RasterLayer &layer,
                              const PointColorMapCatalogSnapshot &colorMaps);

} // namespace pci
