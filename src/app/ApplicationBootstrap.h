#pragma once

#include "app/ApplicationConfig.h"

#include <memory>

namespace pci {

class MainWindow;
class PointColorMapCatalogSnapshot;

[[nodiscard]] std::unique_ptr<MainWindow> bootstrapApplication(
    const ApplicationConfig &config,
    std::shared_ptr<const PointColorMapCatalogSnapshot> colorMaps);

} // namespace pci
