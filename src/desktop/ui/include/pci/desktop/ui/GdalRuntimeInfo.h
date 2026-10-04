#pragma once

#include <QString>

namespace pci {

// What the running GDAL build is and what it can open, carried as plain data.
//
// The UI target must not link GDAL, so the application layer probes once at
// bootstrap and hands the answer down. Reporting it is what lets a user's bug
// report distinguish "this build has no GTI" from "GTI is present and the
// catalog is broken" — a distinction a version number alone cannot make.
struct GdalRuntimeInfo {
    QString version;
    bool tileIndexDriver = false;
    bool virtualRasterDriver = false;
    bool geoPackageDriver = false;
    bool flatGeobufDriver = false;
    bool shapefileDriver = false;
    // False when the probe never ran, which is the case in widget tests that
    // construct a window without a GDAL-backed application.
    bool probed = false;

    [[nodiscard]] bool catalogImport() const noexcept
    {
        return tileIndexDriver &&
               (geoPackageDriver || flatGeobufDriver || shapefileDriver);
    }

    bool operator==(const GdalRuntimeInfo &) const = default;
};

} // namespace pci
