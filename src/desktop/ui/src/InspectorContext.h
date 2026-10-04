#pragma once
#include <pci/document/SceneDocumentSnapshot.h>
namespace pci {
// Values needed by the selected panel, without retaining scene collections.
struct InspectorContext {
    Bounds3d colorBounds;
    PointClassificationFilter classifications =
        PointClassificationFilter::noneVisible();
    std::size_t pointCount = 0;
    std::size_t rasterCount = 0;
    bool allCompatible = true;
    bool anyDifferent = false;
    bool colorizeActive = false;
    bool colorizeCommitting = false;
};
} // namespace pci
