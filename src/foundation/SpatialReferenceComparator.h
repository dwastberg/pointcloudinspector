#pragma once

#include <cstdint>
#include <string>

namespace pci {

enum class SpatialReferenceRelation : std::uint8_t {
    Same,
    Different,
    Unknown,
};

class SpatialReferenceComparator {
public:
    virtual ~SpatialReferenceComparator() = default;
    [[nodiscard]] virtual SpatialReferenceRelation
    compare(const std::string &left, const std::string &right) const = 0;
};

} // namespace pci
