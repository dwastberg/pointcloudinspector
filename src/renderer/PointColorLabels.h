#pragma once

#include "pointcloud/PointColorMapCatalog.h"

#include <QString>

namespace pci {

inline QString pointColorSourceLabel(const PointColorSource source)
{
    switch (source) {
    case PointColorSource::Rgb:
        return QStringLiteral("RGB");
    case PointColorSource::X:
        return QStringLiteral("X");
    case PointColorSource::Y:
        return QStringLiteral("Y");
    case PointColorSource::Z:
        return QStringLiteral("Z");
    case PointColorSource::Intensity:
        return QStringLiteral("Intensity");
    case PointColorSource::Classification:
        return QStringLiteral("Classification");
    case PointColorSource::ReturnNumber:
        return QStringLiteral("Return number");
    case PointColorSource::NumberOfReturns:
        return QStringLiteral("Number of returns");
    }
    return QStringLiteral("Unknown");
}

inline QString pointColorMapLabel(const PointColorMapCatalogSnapshot &catalog,
                                  const PointColorMap colorMap)
{
    const std::string_view name = pointColorMapName(catalog, colorMap);
    return QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
}

} // namespace pci
