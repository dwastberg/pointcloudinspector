#pragma once

#include "app/ApplicationConfig.h"

#include <memory>

namespace pci {

class MainWindow;
class PointColorMapCatalogSnapshot;

// Prints the running GDAL build's version and raster driver availability and
// returns the process exit code: zero when catalog import is available, one
// when it is not. Lives here because this is the layer that links GDAL, and it
// answers before any window or renderer exists so a packaged build can be
// checked on a headless machine.
[[nodiscard]] int reportGdalCapabilities();

[[nodiscard]] std::unique_ptr<MainWindow> bootstrapApplication(
    const ApplicationConfig &config,
    std::shared_ptr<const PointColorMapCatalogSnapshot> colorMaps);

} // namespace pci
