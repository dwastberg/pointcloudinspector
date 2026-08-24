#pragma once

#include "foundation/SpatialReferenceComparator.h"

namespace pci {

class GdalSpatialReferenceComparator final : public SpatialReferenceComparator {
public:
    [[nodiscard]] SpatialReferenceRelation
    compare(const std::string &left, const std::string &right) const override;
};

} // namespace pci
