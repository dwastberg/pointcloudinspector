#pragma once

#include <pci/raster/RasterLayer.h>
#include <pci/vector/VectorLayerStyle.h>

#include <pci/foundation/LayerIdentity.h>

#include <QMetaType>

Q_DECLARE_METATYPE(pci::SceneLayerId)
// Needed for queued delivery and for QSignalSpy to record style edits.
Q_DECLARE_METATYPE(pci::RasterLayerStyle)
Q_DECLARE_METATYPE(pci::VectorLayerStyle)
