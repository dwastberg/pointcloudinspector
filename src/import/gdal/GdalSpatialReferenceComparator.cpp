#include "import/gdal/GdalSpatialReferenceComparator.h"

#include "import/gdal/GdalRuntime.h"

#include <ogr_spatialref.h>

namespace pci {

SpatialReferenceRelation
GdalSpatialReferenceComparator::compare(const std::string &left,
                                        const std::string &right) const
{
    if (left.empty() || right.empty()) {
        return SpatialReferenceRelation::Unknown;
    }
    GdalErrorScope errors;
    OGRSpatialReference leftReference;
    OGRSpatialReference rightReference;
    if (leftReference.importFromWkt(left.c_str()) != OGRERR_NONE ||
        rightReference.importFromWkt(right.c_str()) != OGRERR_NONE) {
        return SpatialReferenceRelation::Unknown;
    }
    return leftReference.IsSame(&rightReference) != 0
               ? SpatialReferenceRelation::Same
               : SpatialReferenceRelation::Different;
}

} // namespace pci
