#pragma once

#include "pointcloud/PointColorMapCatalog.h"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace pci {

struct ParsedCptColorMap {
    std::string name;
    std::string description;
    std::vector<PointColorStop> stops;
};

struct CptColorMapParseError {
    std::size_t line = 0;
    std::string message;
};

using CptColorMapParseResult =
    std::expected<ParsedCptColorMap, CptColorMapParseError>;

// Parses regular GMT color palette tables. RGB, grayscale, HSV, and CMYK
// endpoint colors are converted to normalized RGBA stops. The numeric CPT
// domain is normalized to [0, 1] for the viewer's runtime scalar range.
[[nodiscard]] CptColorMapParseResult
parseCptColorMap(std::string_view contents, std::string_view fallbackName);

} // namespace pci
