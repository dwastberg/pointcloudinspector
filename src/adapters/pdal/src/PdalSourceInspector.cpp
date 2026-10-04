#include <pci/adapters/pdal/PdalSourceInspector.h>

#include <pci/operations/PointCloudImport.h>

#include <pdal/Options.hpp>
#include <pdal/PDALUtils.hpp>
#include <pdal/QuickInfo.hpp>
#include <pdal/StageFactory.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <string_view>

namespace pci {
namespace {

bool endsWithCaseInsensitive(const std::string &value,
                             const std::string_view suffix)
{
    if (value.size() < suffix.size()) {
        return false;
    }
    return std::equal(
        suffix.rbegin(),
        suffix.rend(),
        value.rbegin(),
        [](const char left, const char right) {
            return std::tolower(static_cast<unsigned char>(left)) ==
                   std::tolower(static_cast<unsigned char>(right));
        });
}

std::string inferDriver(pdal::StageFactory &factory,
                        const std::filesystem::path &path)
{
    if (endsWithCaseInsensitive(path.filename().string(), ".copc.laz")) {
        return "readers.copc";
    }
    return factory.inferReaderDriver(path.string());
}

bool containsDimension(const pdal::StringList &dimensions,
                       const std::string_view name)
{
    return std::ranges::find(dimensions, name) != dimensions.end();
}

PointCloudImportError inspectionError(const std::filesystem::path &path,
                                      const std::string_view detail)
{
    return PointCloudImportError("Could not inspect point cloud '" +
                                 path.string() + "': " + std::string(detail));
}

} // namespace

PointCloudMetadata
PdalSourceInspector::inspect(const std::filesystem::path &sourcePath) const
{
    if (sourcePath.empty()) {
        throw inspectionError(sourcePath, "source path is empty");
    }

    try {
        pdal::StageFactory factory;
        const std::string driver = inferDriver(factory, sourcePath);
        if (driver.empty()) {
            throw inspectionError(sourcePath, "unsupported file type");
        }

        pdal::Stage *reader = factory.createStage(driver);
        if (!reader) {
            throw inspectionError(sourcePath,
                                  "PDAL reader is unavailable: " + driver);
        }

        pdal::Options options;
        options.add("filename", sourcePath.string());
        reader->setOptions(options);

        const pdal::QuickInfo info = reader->preview();
        if (!info.valid()) {
            throw inspectionError(sourcePath, "PDAL metadata is invalid");
        }
        if (info.m_pointCount == 0) {
            throw inspectionError(sourcePath, "source contains no points");
        }
        if (!info.m_bounds.valid()) {
            throw inspectionError(sourcePath, "source bounds are invalid");
        }
        if (!containsDimension(info.m_dimNames, "X") ||
            !containsDimension(info.m_dimNames, "Y") ||
            !containsDimension(info.m_dimNames, "Z")) {
            throw inspectionError(sourcePath, "source has no XYZ dimensions");
        }

        PointCloudMetadata metadata{
            .sourcePath = sourcePath,
            .sourceDriver = driver,
            .spatialReferenceWkt =
                info.m_srs.valid() ? info.m_srs.getWKT() : std::string{},
            .sourcePointCount = static_cast<std::uint64_t>(info.m_pointCount),
            .sourceBounds =
                {
                    .minimum =
                        {
                            info.m_bounds.minx,
                            info.m_bounds.miny,
                            info.m_bounds.minz,
                        },
                    .maximum =
                        {
                            info.m_bounds.maxx,
                            info.m_bounds.maxy,
                            info.m_bounds.maxz,
                        },
                },
            .dimensions = info.m_dimNames,
            .hasColor = containsDimension(info.m_dimNames, "Red") &&
                        containsDimension(info.m_dimNames, "Green") &&
                        containsDimension(info.m_dimNames, "Blue"),
            .hasIntensity = containsDimension(info.m_dimNames, "Intensity"),
            .hasClassification =
                containsDimension(info.m_dimNames, "Classification"),
            .hasReturnNumber =
                containsDimension(info.m_dimNames, "ReturnNumber"),
            .hasNumberOfReturns =
                containsDimension(info.m_dimNames, "NumberOfReturns"),
        };
        if (!metadata.sourceBounds.valid()) {
            throw inspectionError(sourcePath, "source bounds are not finite");
        }
        return metadata;
    } catch (const PointCloudImportError &) {
        throw;
    } catch (const std::exception &error) {
        throw inspectionError(sourcePath, error.what());
    }
}

} // namespace pci
