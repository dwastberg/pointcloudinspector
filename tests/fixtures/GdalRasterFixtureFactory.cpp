#include "fixtures/GdalRasterFixtureFactory.h"

#include <cpl_string.h>
#include <gdal_priv.h>
#include <ogr_spatialref.h>

#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace pci::test {
namespace {

struct DatasetCloser {
    void operator()(GDALDataset *dataset) const noexcept
    {
        if (dataset != nullptr) {
            GDALClose(dataset);
        }
    }
};

using DatasetPtr = std::unique_ptr<GDALDataset, DatasetCloser>;

[[nodiscard]] GDALDriver &geoTiffDriver()
{
    GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (driver == nullptr) {
        throw std::runtime_error("the GTiff driver is unavailable");
    }
    return *driver;
}

[[nodiscard]] DatasetPtr create(const std::filesystem::path &path,
                                const int width,
                                const int height,
                                const int bands,
                                const GDALDataType type,
                                const char *const *options = nullptr)
{
    DatasetPtr dataset{geoTiffDriver().Create(path.string().c_str(),
                                              width,
                                              height,
                                              bands,
                                              type,
                                              const_cast<char **>(options))};
    if (dataset == nullptr) {
        throw std::runtime_error("could not create raster fixture " +
                                 path.string());
    }
    return dataset;
}

void applyProjectedReference(GDALDataset &dataset)
{
    OGRSpatialReference reference;
    reference.importFromEPSG(3006);
    reference.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    dataset.SetSpatialRef(&reference);
}

void applyGeographicReference(GDALDataset &dataset)
{
    OGRSpatialReference reference;
    reference.importFromEPSG(4326);
    reference.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    dataset.SetSpatialRef(&reference);
}

void applyTransform(GDALDataset &dataset,
                    const std::array<double, 6> &geoTransform)
{
    std::array<double, 6> mutableTransform = geoTransform;
    if (dataset.SetGeoTransform(mutableTransform.data()) != CE_None) {
        throw std::runtime_error("could not set a fixture geotransform");
    }
}

template <typename Sample>
void writeBand(GDALDataset &dataset,
               const int number,
               const GDALDataType type,
               const std::vector<Sample> &samples)
{
    GDALRasterBand *band = dataset.GetRasterBand(number);
    if (band == nullptr) {
        throw std::runtime_error("fixture band is unavailable");
    }
    std::vector<Sample> writable = samples;
    if (band->RasterIO(GF_Write,
                       0,
                       0,
                       dataset.GetRasterXSize(),
                       dataset.GetRasterYSize(),
                       writable.data(),
                       dataset.GetRasterXSize(),
                       dataset.GetRasterYSize(),
                       type,
                       0,
                       0,
                       nullptr) != CE_None) {
        throw std::runtime_error("could not write a fixture band");
    }
}

void buildOverviews(const std::filesystem::path &path,
                    const std::vector<int> &factors)
{
    DatasetPtr dataset{
        GDALDataset::FromHandle(GDALOpen(path.string().c_str(), GA_Update))};
    if (dataset == nullptr) {
        throw std::runtime_error("could not reopen " + path.string() +
                                 " to build overviews");
    }
    std::vector<int> levels = factors;
    if (dataset->BuildOverviews("AVERAGE",
                                static_cast<int>(levels.size()),
                                levels.data(),
                                0,
                                nullptr,
                                nullptr,
                                nullptr) != CE_None) {
        throw std::runtime_error("could not build fixture overviews");
    }
}

// A diagonal ramp with distinct corner colors, so a mirrored, rotated, or
// half-pixel-shifted placement is visible rather than plausible.
void writeRgb(GDALDataset &dataset, const int width, const int height)
{
    const auto count = static_cast<std::size_t>(width) * height;
    std::vector<unsigned char> red(count);
    std::vector<unsigned char> green(count);
    std::vector<unsigned char> blue(count);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto index = static_cast<std::size_t>(y) * width + x;
            red[index] = static_cast<unsigned char>(x * 255 / (width - 1));
            green[index] = static_cast<unsigned char>(y * 255 / (height - 1));
            blue[index] = 64;
        }
    }
    writeBand(dataset, 1, GDT_Byte, red);
    writeBand(dataset, 2, GDT_Byte, green);
    writeBand(dataset, 3, GDT_Byte, blue);
    dataset.GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
    dataset.GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
    dataset.GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);
}

std::filesystem::path writeRgbFixture(const std::filesystem::path &path)
{
    constexpr int width = 64;
    constexpr int height = 48;
    DatasetPtr dataset = create(path, width, height, 3, GDT_Byte);
    applyProjectedReference(*dataset);
    applyTransform(*dataset, {674000.0, 2.0, 0.0, 6580000.0, 0.0, -2.0});
    writeRgb(*dataset, width, height);
    return path;
}

std::filesystem::path writeOverviewFixture(const std::filesystem::path &path)
{
    constexpr int width = 256;
    constexpr int height = 192;
    {
        DatasetPtr dataset = create(path, width, height, 3, GDT_Byte);
        applyProjectedReference(*dataset);
        applyTransform(*dataset, {674000.0, 1.0, 0.0, 6580000.0, 0.0, -1.0});
        writeRgb(*dataset, width, height);
    }
    // Reductions of 3 and 5 give ratios of 3.0 and 5.12, so nothing downstream
    // can quietly assume a 2^L pyramid.
    buildOverviews(path, {3, 5});
    return path;
}

std::filesystem::path writeRgbaFixture(const std::filesystem::path &path)
{
    constexpr int width = 32;
    constexpr int height = 32;
    DatasetPtr dataset = create(path, width, height, 4, GDT_Byte);
    applyProjectedReference(*dataset);
    applyTransform(*dataset, {674000.0, 1.0, 0.0, 6580000.0, 0.0, -1.0});
    writeRgb(*dataset, width, height);

    const auto count = static_cast<std::size_t>(width) * height;
    std::vector<unsigned char> alpha(count, 255);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width / 2; ++x) {
            alpha[static_cast<std::size_t>(y) * width + x] = 0;
        }
    }
    writeBand(*dataset, 4, GDT_Byte, alpha);
    dataset->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
    return path;
}

std::filesystem::path writeGrayFixture(const std::filesystem::path &path)
{
    constexpr int width = 32;
    constexpr int height = 32;
    DatasetPtr dataset = create(path, width, height, 1, GDT_Byte);
    applyProjectedReference(*dataset);
    applyTransform(*dataset, {674000.0, 1.0, 0.0, 6580000.0, 0.0, -1.0});

    std::vector<unsigned char> gray(static_cast<std::size_t>(width) * height);
    for (std::size_t index = 0; index < gray.size(); ++index) {
        gray[index] = static_cast<unsigned char>(index % 256);
    }
    writeBand(*dataset, 1, GDT_Byte, gray);
    dataset->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex);
    return path;
}

std::filesystem::path writePaletteFixture(const std::filesystem::path &path)
{
    constexpr int width = 32;
    constexpr int height = 32;
    DatasetPtr dataset = create(path, width, height, 1, GDT_Byte);
    applyProjectedReference(*dataset);
    applyTransform(*dataset, {674000.0, 1.0, 0.0, 6580000.0, 0.0, -1.0});

    GDALColorTable table;
    const std::array<GDALColorEntry, 4> entries{
        GDALColorEntry{255, 0, 0, 255},
        GDALColorEntry{0, 255, 0, 255},
        GDALColorEntry{0, 0, 255, 255},
        GDALColorEntry{0, 0, 0, 0},
    };
    for (int entry = 0; entry < static_cast<int>(entries.size()); ++entry) {
        table.SetColorEntry(entry, &entries[static_cast<std::size_t>(entry)]);
    }
    // The color table has to be declared before any pixel write: GTiff cannot
    // change its photometric interpretation once data is on disk.
    dataset->GetRasterBand(1)->SetColorTable(&table);
    dataset->GetRasterBand(1)->SetColorInterpretation(GCI_PaletteIndex);

    std::vector<unsigned char> indices(static_cast<std::size_t>(width) *
                                       height);
    for (std::size_t index = 0; index < indices.size(); ++index) {
        indices[index] = static_cast<unsigned char>(index % 4);
    }
    writeBand(*dataset, 1, GDT_Byte, indices);
    return path;
}

std::filesystem::path writeTerrainFixture(const std::filesystem::path &path)
{
    constexpr int width = 64;
    constexpr int height = 64;
    constexpr double nodata = -9999.0;
    DatasetPtr dataset = create(path, width, height, 1, GDT_Float32);
    applyProjectedReference(*dataset);
    applyTransform(*dataset, {674000.0, 1.0, 0.0, 6580000.0, 0.0, -1.0});

    std::vector<float> elevation(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto index = static_cast<std::size_t>(y) * width + x;
            // A nodata block against valid terrain, so filtering across the
            // boundary can be checked for color bleed.
            elevation[index] = x < 8 ? static_cast<float>(nodata)
                                     : 100.0F + static_cast<float>(x + y);
        }
    }
    writeBand(*dataset, 1, GDT_Float32, elevation);
    dataset->GetRasterBand(1)->SetNoDataValue(nodata);
    return path;
}

std::filesystem::path writeUnsigned16Fixture(const std::filesystem::path &path)
{
    constexpr int width = 64;
    constexpr int height = 64;
    DatasetPtr dataset = create(path, width, height, 1, GDT_UInt16);
    applyProjectedReference(*dataset);
    applyTransform(*dataset, {674000.0, 1.0, 0.0, 6580000.0, 0.0, -1.0});

    // Occupies roughly 1000-5000 of the 0-65535 domain, which a full-range map
    // would render nearly black.
    std::vector<unsigned short> samples(static_cast<std::size_t>(width) *
                                        height);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        samples[index] = static_cast<unsigned short>(1000 + (index % 4001));
    }
    writeBand(*dataset, 1, GDT_UInt16, samples);
    dataset->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex);
    return path;
}

std::filesystem::path writeRotatedFixture(const std::filesystem::path &path)
{
    constexpr int width = 16;
    constexpr int height = 16;
    DatasetPtr dataset = create(path, width, height, 3, GDT_Byte);
    applyProjectedReference(*dataset);
    // A 30 degree rotation with a negative pixel height.
    applyTransform(*dataset, {674000.0, 1.732, -0.5, 6580000.0, 1.0, -0.866});
    writeRgb(*dataset, width, height);
    return path;
}

std::filesystem::path
writeAntimeridianFixture(const std::filesystem::path &path)
{
    constexpr int width = 32;
    constexpr int height = 16;
    DatasetPtr dataset = create(path, width, height, 1, GDT_Byte);
    applyGeographicReference(*dataset);
    // Spans -179.5 to 179.5 degrees: a min/max box that wraps the globe.
    applyTransform(*dataset,
                   {-179.5, 359.0 / width, 0.0, 10.0, 0.0, -20.0 / height});

    std::vector<unsigned char> gray(static_cast<std::size_t>(width) * height,
                                    128);
    writeBand(*dataset, 1, GDT_Byte, gray);
    dataset->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex);
    return path;
}

// An ordinary un-overviewed mid-size orthophoto: the most common thing a user
// drags in first. Written sparsely so the fixture costs nothing on disk while
// its logical size still exercises the bounded base-band path.
std::filesystem::path writeMidSizeFixture(const std::filesystem::path &path)
{
    constexpr int extent = 5000;
    const std::array<const char *, 4> options{
        "TILED=YES", "SPARSE_OK=TRUE", "COMPRESS=DEFLATE", nullptr};
    DatasetPtr dataset =
        create(path, extent, extent, 3, GDT_Byte, options.data());
    applyProjectedReference(*dataset);
    applyTransform(*dataset, {674000.0, 0.2, 0.0, 6580000.0, 0.0, -0.2});
    dataset->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
    dataset->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
    dataset->GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);
    return path;
}

std::filesystem::path writeSparseHugeFixture(const std::filesystem::path &path)
{
    // Enormous logically, empty physically. Inspecting it must cost the sample
    // budget rather than the pixel count.
    constexpr int extent = 100000;
    const std::array<const char *, 5> options{"TILED=YES",
                                              "SPARSE_OK=TRUE",
                                              "BIGTIFF=YES",
                                              "BLOCKXSIZE=256",
                                              nullptr};
    DatasetPtr dataset =
        create(path, extent, extent, 1, GDT_Float32, options.data());
    applyProjectedReference(*dataset);
    applyTransform(*dataset, {674000.0, 0.1, 0.0, 6580000.0, 0.0, -0.1});
    return path;
}

// A VRT whose red band declares an explicit overview while green and blue
// declare none. Only band 1 can offer the 32x24 level, so the intersection has
// something real to reject: reading color from one overview size and the rest
// from another would misregister them.
std::filesystem::path
writeMismatchedOverviewFixture(const std::filesystem::path &path,
                               const std::filesystem::path &source,
                               const std::filesystem::path &overview)
{
    const auto band = [&source](const int number,
                                const char *interpretation,
                                const std::string &extra) {
        return "  <VRTRasterBand dataType=\"Byte\" band=\"" +
               std::to_string(number) + "\">\n    <ColorInterp>" +
               interpretation +
               "</ColorInterp>\n"
               "    <SimpleSource>\n"
               "      <SourceFilename relativeToVRT=\"0\">" +
               source.string() +
               "</SourceFilename>\n"
               "      <SourceBand>" +
               std::to_string(number) +
               "</SourceBand>\n"
               "    </SimpleSource>\n" +
               extra + "  </VRTRasterBand>\n";
    };

    const std::string redOverview =
        "    <Overview>\n"
        "      <SourceFilename relativeToVRT=\"0\">" +
        overview.string() +
        "</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </Overview>\n";

    const std::string document =
        "<VRTDataset rasterXSize=\"64\" rasterYSize=\"48\">\n"
        "  <SRS>EPSG:3006</SRS>\n"
        "  <GeoTransform>674000.0, 2.0, 0.0, 6580000.0, 0.0, -2.0"
        "</GeoTransform>\n" +
        band(1, "Red", redOverview) + band(2, "Green", {}) +
        band(3, "Blue", {}) + "</VRTDataset>\n";

    VSILFILE *file = VSIFOpenL(path.string().c_str(), "wb");
    if (file == nullptr) {
        throw std::runtime_error("could not write " + path.string());
    }
    VSIFWriteL(document.data(), 1, document.size(), file);
    VSIFCloseL(file);
    return path;
}

// A standalone half-resolution image used as an explicit VRT overview.
std::filesystem::path writeOverviewImage(const std::filesystem::path &path)
{
    constexpr int width = 32;
    constexpr int height = 24;
    DatasetPtr dataset = create(path, width, height, 1, GDT_Byte);
    std::vector<unsigned char> gray(static_cast<std::size_t>(width) * height,
                                    100);
    writeBand(*dataset, 1, GDT_Byte, gray);
    return path;
}

} // namespace

GdalRasterFixturePaths
writeGdalRasterFixtures(const std::filesystem::path &directory)
{
    GDALAllRegister();
    std::filesystem::create_directories(directory);

    GdalRasterFixturePaths paths;
    paths.rgb = writeRgbFixture(directory / "rgb.tif");
    paths.rgbNonPowerOfTwoOverviews =
        writeOverviewFixture(directory / "rgb-overviews.tif");
    paths.rgba = writeRgbaFixture(directory / "rgba.tif");
    paths.gray = writeGrayFixture(directory / "gray.tif");
    paths.palette = writePaletteFixture(directory / "palette.tif");
    paths.terrain = writeTerrainFixture(directory / "terrain.tif");
    paths.unsigned16 = writeUnsigned16Fixture(directory / "uint16.tif");
    paths.rotated = writeRotatedFixture(directory / "rotated.tif");
    paths.antimeridian =
        writeAntimeridianFixture(directory / "antimeridian.tif");
    paths.midSizeNoOverviews = writeMidSizeFixture(directory / "mid-size.tif");
    paths.sparseHuge = writeSparseHugeFixture(directory / "sparse-huge.tif");

    paths.mismatchedOverviews = writeMismatchedOverviewFixture(
        directory / "mismatched-overviews.vrt",
        paths.rgb,
        writeOverviewImage(directory / "red-overview.tif"));
    return paths;
}

} // namespace pci::test
