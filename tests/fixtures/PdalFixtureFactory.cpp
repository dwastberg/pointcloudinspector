#include "fixtures/PdalFixtureFactory.h"

#include <pdal/Dimension.hpp>
#include <pdal/Options.hpp>
#include <pdal/PointTable.hpp>
#include <pdal/PointView.hpp>
#include <pdal/SpatialReference.hpp>
#include <pdal/StageFactory.hpp>
#include <pdal/io/BufferReader.hpp>

#include <stdexcept>
#include <string>

namespace pci::test {
namespace {

void registerFixtureDimensions(pdal::PointLayout &layout)
{
    layout.registerDims({
        pdal::Dimension::Id::X,
        pdal::Dimension::Id::Y,
        pdal::Dimension::Id::Z,
        pdal::Dimension::Id::Red,
        pdal::Dimension::Id::Green,
        pdal::Dimension::Id::Blue,
        pdal::Dimension::Id::Intensity,
        pdal::Dimension::Id::Classification,
        pdal::Dimension::Id::ReturnNumber,
        pdal::Dimension::Id::NumberOfReturns,
    });
}

pdal::PointViewPtr createFixtureView(pdal::PointTable &table)
{
    registerFixtureDimensions(*table.layout());

    auto view = std::make_shared<pdal::PointView>(table);
    for (pdal::PointId index = 0; index < fixturePoints.size(); ++index) {
        const FixturePoint &point = fixturePoints[index];
        view->setField(pdal::Dimension::Id::X, index, point.x);
        view->setField(pdal::Dimension::Id::Y, index, point.y);
        view->setField(pdal::Dimension::Id::Z, index, point.z);
        view->setField(pdal::Dimension::Id::Red, index, point.red);
        view->setField(pdal::Dimension::Id::Green, index, point.green);
        view->setField(pdal::Dimension::Id::Blue, index, point.blue);
        view->setField(pdal::Dimension::Id::Intensity, index, point.intensity);
        view->setField(
            pdal::Dimension::Id::Classification, index, point.classification);
        view->setField(
            pdal::Dimension::Id::ReturnNumber, index, point.returnNumber);
        view->setField(
            pdal::Dimension::Id::NumberOfReturns, index, point.numberOfReturns);
    }
    return view;
}

pdal::PointViewPtr createResidencyStressView(pdal::PointTable &table)
{
    registerFixtureDimensions(*table.layout());
    auto view = std::make_shared<pdal::PointView>(table);
    constexpr pdal::PointId edge = 40;
    constexpr pdal::PointId pointCount = edge * edge * edge;
    for (pdal::PointId index = 0; index < pointCount; ++index) {
        const pdal::PointId x = index % edge;
        const pdal::PointId y = (index / edge) % edge;
        const pdal::PointId z = index / (edge * edge);
        view->setField(
            pdal::Dimension::Id::X, index, 1000.0 + static_cast<double>(x));
        view->setField(
            pdal::Dimension::Id::Y, index, 2000.0 + static_cast<double>(y));
        view->setField(
            pdal::Dimension::Id::Z, index, 10.0 + static_cast<double>(z));
        view->setField(pdal::Dimension::Id::Red,
                       index,
                       static_cast<std::uint16_t>(x * 1600U));
        view->setField(pdal::Dimension::Id::Green,
                       index,
                       static_cast<std::uint16_t>(y * 1600U));
        view->setField(pdal::Dimension::Id::Blue,
                       index,
                       static_cast<std::uint16_t>(z * 1600U));
        view->setField(pdal::Dimension::Id::Intensity,
                       index,
                       static_cast<std::uint16_t>((x + y + z) * 500U));
        view->setField(pdal::Dimension::Id::Classification,
                       index,
                       static_cast<std::uint8_t>(2U + index % 5U));
        view->setField(
            pdal::Dimension::Id::ReturnNumber, index, std::uint8_t{1});
        view->setField(
            pdal::Dimension::Id::NumberOfReturns, index, std::uint8_t{1});
    }
    return view;
}

void writeFixture(const std::filesystem::path &path,
                  const std::string &writerDriver,
                  const bool compressed,
                  const bool residencyStress = false)
{
    pdal::PointTable table;
    const pdal::PointViewPtr view = residencyStress
                                        ? createResidencyStressView(table)
                                        : createFixtureView(table);

    pdal::BufferReader reader;
    reader.addView(view);
    reader.setSpatialReference(pdal::SpatialReference("EPSG:3006"));

    pdal::StageFactory factory;
    pdal::Stage *writer = factory.createStage(writerDriver);
    if (!writer) {
        throw std::runtime_error("PDAL stage is unavailable: " + writerDriver);
    }

    pdal::Options options;
    options.add("filename", path.string());
    options.add("a_srs", "EPSG:3006");
    options.add("scale_x", 0.01);
    options.add("scale_y", 0.01);
    options.add("scale_z", 0.01);
    if (writerDriver == "writers.las") {
        options.add("minor_version", 4);
        options.add("dataformat_id", 7);
        options.add("compression", compressed);
    }
    writer->setOptions(options);
    writer->setInput(reader);
    writer->prepare(table);
    writer->execute(table);
}

} // namespace

PdalFixturePaths writePdalFixtures(const std::filesystem::path &directory)
{
    std::filesystem::create_directories(directory);
    const PdalFixturePaths paths{
        .las = directory / "fixture.las",
        .laz = directory / "fixture.laz",
        .copc = directory / "fixture.copc.laz",
    };

    writeFixture(paths.las, "writers.las", false);
    writeFixture(paths.laz, "writers.las", true);
    writeFixture(paths.copc, "writers.copc", true);
    return paths;
}

std::filesystem::path
writePdalResidencyStressFixture(const std::filesystem::path &directory)
{
    std::filesystem::create_directories(directory);
    const std::filesystem::path path = directory / "residency-stress.copc.laz";
    writeFixture(path, "writers.copc", true, true);
    return path;
}

} // namespace pci::test
