#include "fixtures/GdalRasterFixtureFactory.h"

#include <cpl_string.h>
#include <gdal_priv.h>
#include <gdal_utils.h>
#include <ogr_spatialref.h>

#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
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

void buildOverviews(const std::filesystem::path &path, std::vector<int> levels)
{
    DatasetPtr dataset{
        GDALDataset::FromHandle(GDALOpen(path.string().c_str(), GA_Update))};
    if (dataset == nullptr) {
        throw std::runtime_error("could not reopen " + path.string() +
                                 " to build overviews");
    }
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

void writeTextFile(const std::filesystem::path &path,
                   const std::string &contents)
{
    VSILFILE *file = VSIFOpenL(path.string().c_str(), "wb");
    if (file == nullptr) {
        throw std::runtime_error("could not write " + path.string());
    }
    const std::size_t written =
        VSIFWriteL(contents.data(), 1, contents.size(), file);
    VSIFCloseL(file);
    if (written != contents.size()) {
        throw std::runtime_error("could not finish " + path.string());
    }
}

void addDatasetMask(const std::filesystem::path &path,
                    const int width,
                    const int height)
{
    DatasetPtr dataset{
        GDALDataset::FromHandle(GDALOpen(path.string().c_str(), GA_Update))};
    if (!dataset) {
        throw std::runtime_error("could not reopen masked fixture");
    }
    GDALRasterBand *color = dataset->GetRasterBand(1);
    if (color == nullptr || color->CreateMaskBand(GMF_PER_DATASET) != CE_None) {
        throw std::runtime_error("could not create dataset mask");
    }
    GDALRasterBand *mask = color->GetMaskBand();
    if (mask == nullptr) {
        throw std::runtime_error("dataset mask is unavailable");
    }
    std::vector<unsigned char> validity(
        static_cast<std::size_t>(width) * height, 255);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width / 4; ++x) {
            validity[static_cast<std::size_t>(y) * width + x] = 0;
        }
    }
    if (mask->RasterIO(GF_Write,
                       0,
                       0,
                       width,
                       height,
                       validity.data(),
                       width,
                       height,
                       GDT_Byte,
                       0,
                       0,
                       nullptr) != CE_None) {
        throw std::runtime_error("could not write dataset mask");
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

std::filesystem::path
writePointAlignedFixture(const std::filesystem::path &path,
                         const bool partialAlpha)
{
    constexpr int width = 32;
    constexpr int height = 32;
    DatasetPtr dataset =
        create(path, width, height, partialAlpha ? 4 : 3, GDT_Byte);
    applyProjectedReference(*dataset);
    applyTransform(*dataset, {995.0, 1.0, 0.0, 2015.0, 0.0, -1.0});
    const std::size_t count = static_cast<std::size_t>(width) * height;
    std::vector<unsigned char> red(count);
    std::vector<unsigned char> green(count);
    std::vector<unsigned char> blue(count, 64);
    std::vector<unsigned char> alpha(count);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            red[index] = static_cast<unsigned char>(x * 8);
            green[index] = static_cast<unsigned char>(y * 8);
            alpha[index] =
                static_cast<unsigned char>(1 + (x + y * width) % 254);
        }
    }
    writeBand(*dataset, 1, GDT_Byte, red);
    writeBand(*dataset, 2, GDT_Byte, green);
    writeBand(*dataset, 3, GDT_Byte, blue);
    dataset->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
    dataset->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
    dataset->GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);
    if (partialAlpha) {
        writeBand(*dataset, 4, GDT_Byte, alpha);
        dataset->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
    }
    return path;
}

std::filesystem::path
writePositionalRgbFixture(const std::filesystem::path &path)
{
    constexpr int width = 32;
    constexpr int height = 24;
    std::filesystem::path source = path;
    source.replace_extension(".tif");
    {
        DatasetPtr dataset = create(source, width, height, 3, GDT_Byte);
        applyProjectedReference(*dataset);
        applyTransform(*dataset, {674000.0, 2.0, 0.0, 6580000.0, 0.0, -2.0});
        writeRgb(*dataset, width, height);
    }

    // GTiff persists three Byte bands as RGB through its PHOTOMETRIC tag even
    // after SetColorInterpretation(Undefined), so reopening that file cannot
    // exercise the positional fallback. A VRT with deliberately unlabelled
    // bands preserves the real-world ambiguous layout deterministically.
    const auto band = [&source](const int number) {
        return "  <VRTRasterBand dataType=\"Byte\" band=\"" +
               std::to_string(number) +
               "\">\n"
               "    <SimpleSource>\n"
               "      <SourceFilename relativeToVRT=\"0\">" +
               source.string() +
               "</SourceFilename>\n"
               "      <SourceBand>" +
               std::to_string(number) +
               "</SourceBand>\n"
               "    </SimpleSource>\n"
               "  </VRTRasterBand>\n";
    };
    const std::string document =
        "<VRTDataset rasterXSize=\"32\" rasterYSize=\"24\">\n"
        "  <SRS>EPSG:3006</SRS>\n"
        "  <GeoTransform>674000.0, 2.0, 0.0, 6580000.0, 0.0, -2.0"
        "</GeoTransform>\n" +
        band(1) + band(2) + band(3) + "</VRTDataset>\n";
    VSILFILE *file = VSIFOpenL(path.string().c_str(), "wb");
    if (file == nullptr) {
        throw std::runtime_error("could not write " + path.string());
    }
    VSIFWriteL(document.data(), 1, document.size(), file);
    VSIFCloseL(file);
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

std::filesystem::path writeMaskedFixture(const std::filesystem::path &path,
                                         const int width = 64,
                                         const int height = 48)
{
    std::error_code cleanupError;
    std::filesystem::remove(path.string() + ".msk", cleanupError);
    {
        DatasetPtr dataset = create(path, width, height, 3, GDT_Byte);
        applyProjectedReference(*dataset);
        applyTransform(*dataset, {674000.0, 1.0, 0.0, 6580000.0, 0.0, -1.0});
        writeRgb(*dataset, width, height);
    }
    addDatasetMask(path, width, height);
    return path;
}

std::filesystem::path
writeCoverageContinuousFixture(const std::filesystem::path &path)
{
    constexpr int extent = 512;
    DatasetPtr dataset = create(path, extent, extent, 1, GDT_Byte);
    applyProjectedReference(*dataset);
    applyTransform(*dataset, {674000.0, 1.0, 0.0, 6580000.0, 0.0, -1.0});
    std::vector<unsigned char> samples(static_cast<std::size_t>(extent) *
                                       extent);
    for (int y = 0; y < extent; ++y) {
        for (int x = 0; x < extent; ++x) {
            samples[static_cast<std::size_t>(y) * extent + x] =
                x % 2 == 0 ? 0 : 255;
        }
    }
    writeBand(*dataset, 1, GDT_Byte, samples);
    dataset->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex);
    return path;
}

std::filesystem::path
writeCoverageCategoricalFixture(const std::filesystem::path &path)
{
    constexpr int extent = 512;
    DatasetPtr dataset = create(path, extent, extent, 1, GDT_Byte);
    applyProjectedReference(*dataset);
    applyTransform(*dataset, {674000.0, 1.0, 0.0, 6580000.0, 0.0, -1.0});
    GDALColorTable table;
    const std::array<GDALColorEntry, 3> entries{
        GDALColorEntry{255, 0, 0, 255},
        GDALColorEntry{0, 255, 0, 255},
        GDALColorEntry{0, 0, 255, 255},
    };
    for (int entry = 0; entry < static_cast<int>(entries.size()); ++entry) {
        table.SetColorEntry(entry, &entries[static_cast<std::size_t>(entry)]);
    }
    GDALRasterBand *band = dataset->GetRasterBand(1);
    band->SetColorTable(&table);
    band->SetColorInterpretation(GCI_PaletteIndex);
    std::vector<unsigned char> indices(static_cast<std::size_t>(extent) *
                                       extent);
    for (int y = 0; y < extent; ++y) {
        for (int x = 0; x < extent; ++x) {
            indices[static_cast<std::size_t>(y) * extent + x] =
                x % 2 == 0 ? 0 : 2;
        }
    }
    writeBand(*dataset, 1, GDT_Byte, indices);
    return path;
}

std::filesystem::path
writeCoverageNodataFixture(const std::filesystem::path &path)
{
    constexpr int extent = 512;
    constexpr float nodata = -9999.0F;
    DatasetPtr dataset = create(path, extent, extent, 1, GDT_Float32);
    applyProjectedReference(*dataset);
    applyTransform(*dataset, {674000.0, 1.0, 0.0, 6580000.0, 0.0, -1.0});
    std::vector<float> samples(static_cast<std::size_t>(extent) * extent);
    for (int y = 0; y < extent; ++y) {
        for (int x = 0; x < extent; ++x) {
            samples[static_cast<std::size_t>(y) * extent + x] =
                x < extent / 4 ? nodata : 100.0F + x + y;
        }
    }
    writeBand(*dataset, 1, GDT_Float32, samples);
    dataset->GetRasterBand(1)->SetNoDataValue(nodata);
    return path;
}

std::filesystem::path writeWorldFileFixture(const std::filesystem::path &path)
{
    constexpr int width = 16;
    constexpr int height = 12;
    {
        DatasetPtr dataset = create(path, width, height, 3, GDT_Byte);
        writeRgb(*dataset, width, height);
    }
    std::filesystem::path worldFile = path;
    worldFile.replace_extension(".tfw");
    // World files name the center of the upper-left pixel. GDAL converts this
    // to the pixel-edge origin 674000,6580000 in its six-term transform.
    writeTextFile(worldFile, "2.0\n0.0\n0.0\n-2.0\n674001.0\n6579999.0\n");
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
    {
        DatasetPtr dataset = create(path, width, height, 1, GDT_Float32);
        applyProjectedReference(*dataset);
        applyTransform(*dataset, {674000.0, 1.0, 0.0, 6580000.0, 0.0, -1.0});

        std::vector<float> elevation(static_cast<std::size_t>(width) * height);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const auto index = static_cast<std::size_t>(y) * width + x;
                // A nodata block against valid terrain, so filtering across
                // the downsampled boundary can be checked for color bleed.
                elevation[index] = x < 8 ? static_cast<float>(nodata)
                                         : 100.0F + static_cast<float>(x + y);
            }
        }
        writeBand(*dataset, 1, GDT_Float32, elevation);
        dataset->GetRasterBand(1)->SetNoDataValue(nodata);
    }
    buildOverviews(path, {2});
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

// One member of a catalog: a small tile placed at a known origin. Members stay
// tiny on disk while their placement makes the catalog's logical extent large,
// which is what lets the acceptance test fix an enormous size without writing
// an enormous file.
[[nodiscard]] std::filesystem::path
writeCatalogMember(const std::filesystem::path &path,
                   const double originX,
                   const double originY,
                   const double pixelSize,
                   const int extent,
                   const std::uint8_t tint)
{
    const std::array<const char *, 3> options{
        "TILED=YES", "BLOCKXSIZE=256", nullptr};
    DatasetPtr dataset =
        create(path, extent, extent, 3, GDT_Byte, options.data());
    applyProjectedReference(*dataset);
    applyTransform(*dataset,
                   {originX, pixelSize, 0.0, originY, 0.0, -pixelSize});

    // A checkerboard at the finest scale: an overview cannot reproduce it, so
    // reaching it on screen proves the native level was read.
    std::vector<std::uint8_t> row(static_cast<std::size_t>(extent));
    for (int band = 1; band <= 3; ++band) {
        dataset->GetRasterBand(band)->SetColorInterpretation(
            band == 1   ? GCI_RedBand
            : band == 2 ? GCI_GreenBand
                        : GCI_BlueBand);
        for (int y = 0; y < extent; ++y) {
            for (int x = 0; x < extent; ++x) {
                const bool even = ((x / 2) + (y / 2)) % 2 == 0;
                row[static_cast<std::size_t>(x)] =
                    even ? tint : static_cast<std::uint8_t>(255U - tint);
            }
            if (dataset->GetRasterBand(band)->RasterIO(GF_Write,
                                                       0,
                                                       y,
                                                       extent,
                                                       1,
                                                       row.data(),
                                                       extent,
                                                       1,
                                                       GDT_Byte,
                                                       0,
                                                       0) != CE_None) {
                throw std::runtime_error("could not write a catalog member");
            }
        }
    }

    // Members carry their own overviews so the catalog can offer coarse
    // coverage instead of only native-resolution reads.
    const std::array<int, 3> levels{2, 4, 8};
    if (dataset->BuildOverviews("AVERAGE",
                                static_cast<int>(levels.size()),
                                const_cast<int *>(levels.data()),
                                0,
                                nullptr,
                                nullptr,
                                nullptr) != CE_None) {
        throw std::runtime_error("could not build catalog member overviews");
    }
    return path;
}

// A VRT mosaic over separate members, built by GDAL itself rather than by
// hand-written XML, so the fixture exercises the same path a user's mosaic
// takes.
std::filesystem::path
writeVrtMosaicFixture(const std::filesystem::path &path,
                      const std::vector<std::filesystem::path> &members)
{
    std::vector<GDALDatasetH> sources;
    std::vector<DatasetPtr> owned;
    for (const std::filesystem::path &member : members) {
        DatasetPtr dataset{
            GDALDataset::FromHandle(GDALOpenEx(member.string().c_str(),
                                               GDAL_OF_RASTER,
                                               nullptr,
                                               nullptr,
                                               nullptr))};
        if (!dataset) {
            throw std::runtime_error("could not open a VRT member");
        }
        sources.push_back(GDALDataset::ToHandle(dataset.get()));
        owned.push_back(std::move(dataset));
    }
    DatasetPtr result{
        GDALDataset::FromHandle(GDALBuildVRT(path.string().c_str(),
                                             static_cast<int>(sources.size()),
                                             sources.data(),
                                             nullptr,
                                             nullptr,
                                             nullptr))};
    if (!result) {
        throw std::runtime_error("could not build the VRT mosaic fixture");
    }
    return path;
}

// A GTI catalog over the same members. Built through GDAL's own tile-index
// utility, so the index layout matches what gdaltindex produces rather than a
// hand-rolled approximation that might diverge from the driver's expectations.
std::filesystem::path
writeCatalogFixture(const std::filesystem::path &path,
                    const std::vector<std::filesystem::path> &members)
{
    // Built through GDAL's own tile-index utility so the index layout matches
    // what gdaltindex produces, rather than a hand-rolled approximation that
    // could diverge from what the driver expects.
    std::vector<std::string> names;
    std::vector<const char *> sources;
    names.reserve(members.size());
    sources.reserve(members.size() + 1);
    for (const std::filesystem::path &member : members) {
        names.push_back(member.string());
    }
    for (const std::string &name : names) {
        sources.push_back(name.c_str());
    }
    sources.push_back(nullptr);

    // A pre-existing index would be appended to, so a rebuilt corpus would
    // accumulate duplicate members and stop being deterministic.
    std::error_code removeError;
    std::filesystem::remove(path, removeError);

    GDALTileIndexOptions *options = GDALTileIndexOptionsNew(nullptr, nullptr);
    if (options == nullptr) {
        throw std::runtime_error("could not build tile index options");
    }
    DatasetPtr result{
        GDALDataset::FromHandle(GDALTileIndex(path.string().c_str(),
                                              static_cast<int>(names.size()),
                                              sources.data(),
                                              options,
                                              nullptr))};
    GDALTileIndexOptionsFree(options);
    if (!result) {
        throw std::runtime_error("could not build the GTI catalog fixture");
    }
    return path;
}

// A catalog does not automatically publish the overviews stored inside its
// members. Supply a small, real external overview for the logical catalog so
// tests can prove progressive coverage without asking GDAL to resample the
// 40-billion-pixel virtual base during fixture setup.
void writeCatalogOverview(const std::filesystem::path &catalog)
{
    constexpr int baseExtent = 200512;
    constexpr int reduction = 128;
    constexpr int overviewExtent = (baseExtent + reduction - 1) / reduction;
    constexpr int memberBaseExtent = 512;
    constexpr int memberBaseSpacing = 200000;

    std::filesystem::path overview = catalog;
    overview += ".ovr";
    std::error_code removeError;
    std::filesystem::remove(overview, removeError);

    const std::array<const char *, 4> options{
        "TILED=YES", "SPARSE_OK=TRUE", "BLOCKXSIZE=256", nullptr};
    DatasetPtr dataset = create(
        overview, overviewExtent, overviewExtent, 3, GDT_Byte, options.data());
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(overviewExtent) * overviewExtent, 0);
    for (int member = 0; member < 4; ++member) {
        const int baseX = (member % 2) * memberBaseSpacing;
        const int baseY = (member / 2) * memberBaseSpacing;
        const int firstX = baseX / reduction;
        const int firstY = baseY / reduction;
        const int lastX =
            (baseX + memberBaseExtent + reduction - 1) / reduction;
        const int lastY =
            (baseY + memberBaseExtent + reduction - 1) / reduction;
        for (int y = firstY; y < lastY; ++y) {
            for (int x = firstX; x < lastX; ++x) {
                pixels[static_cast<std::size_t>(y) * overviewExtent + x] = 127;
            }
        }
    }
    for (int band = 1; band <= 3; ++band) {
        writeBand(*dataset, band, GDT_Byte, pixels);
        dataset->GetRasterBand(band)->SetColorInterpretation(
            band == 1   ? GCI_RedBand
            : band == 2 ? GCI_GreenBand
                        : GCI_BlueBand);
    }
    dataset->GetRasterBand(1)->SetMetadataItem("RESAMPLING", "AVERAGE");
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

std::filesystem::path writeMaskImage(const std::filesystem::path &path)
{
    constexpr int width = 64;
    constexpr int height = 48;
    DatasetPtr dataset = create(path, width, height, 1, GDT_Byte);
    std::vector<unsigned char> mask(static_cast<std::size_t>(width) * height,
                                    255);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width / 4; ++x) {
            mask[static_cast<std::size_t>(y) * width + x] = 0;
        }
    }
    writeBand(*dataset, 1, GDT_Byte, mask);
    return path;
}

std::filesystem::path
writeMismatchedMaskOverviewFixture(const std::filesystem::path &path,
                                   const std::filesystem::path &source,
                                   const std::filesystem::path &overview,
                                   const std::filesystem::path &mask)
{
    const auto band = [&](const int number, const char *interpretation) {
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
               "    </SimpleSource>\n"
               "    <Overview>\n"
               "      <SourceFilename relativeToVRT=\"0\">" +
               overview.string() +
               "</SourceFilename>\n"
               "      <SourceBand>1</SourceBand>\n"
               "    </Overview>\n"
               "  </VRTRasterBand>\n";
    };
    const std::string document =
        "<VRTDataset rasterXSize=\"64\" rasterYSize=\"48\">\n"
        "  <SRS>EPSG:3006</SRS>\n"
        "  <GeoTransform>674000.0, 2.0, 0.0, 6580000.0, 0.0, -2.0"
        "</GeoTransform>\n" +
        band(1, "Red") + band(2, "Green") + band(3, "Blue") +
        "  <MaskBand>\n"
        "    <VRTRasterBand dataType=\"Byte\">\n"
        "      <SimpleSource>\n"
        "        <SourceFilename relativeToVRT=\"0\">" +
        mask.string() +
        "</SourceFilename>\n"
        "        <SourceBand>1</SourceBand>\n"
        "      </SimpleSource>\n"
        "    </VRTRasterBand>\n"
        "  </MaskBand>\n"
        "</VRTDataset>\n";
    writeTextFile(path, document);
    return path;
}

} // namespace

GdalRasterFixturePaths
writeGdalRasterFixtures(const std::filesystem::path &directory)
{
    GDALAllRegister();
    std::filesystem::create_directories(directory);

    GdalRasterFixturePaths paths;
    paths.pointAligned =
        writePointAlignedFixture(directory / "point-aligned.tif", false);
    paths.pointAlignedPartialAlpha = writePointAlignedFixture(
        directory / "point-aligned-partial-alpha.tif", true);
    paths.rgb = writeRgbFixture(directory / "rgb.tif");
    paths.positionalRgb =
        writePositionalRgbFixture(directory / "positional-rgb.vrt");
    paths.rgbNonPowerOfTwoOverviews =
        writeOverviewFixture(directory / "rgb-overviews.tif");
    paths.rgba = writeRgbaFixture(directory / "rgba.tif");
    paths.masked = writeMaskedFixture(directory / "masked.tif");
    paths.gray = writeGrayFixture(directory / "gray.tif");
    paths.palette = writePaletteFixture(directory / "palette.tif");
    paths.coverageContinuous =
        writeCoverageContinuousFixture(directory / "coverage-continuous.tif");
    paths.coverageCategorical =
        writeCoverageCategoricalFixture(directory / "coverage-categorical.tif");
    paths.coverageMasked =
        writeMaskedFixture(directory / "coverage-masked.tif", 512, 512);
    paths.coverageNodata =
        writeCoverageNodataFixture(directory / "coverage-nodata.tif");
    paths.terrain = writeTerrainFixture(directory / "terrain.tif");
    paths.unsigned16 = writeUnsigned16Fixture(directory / "uint16.tif");
    paths.rotated = writeRotatedFixture(directory / "rotated.tif");
    paths.worldFile = writeWorldFileFixture(directory / "world-file.tif");
    paths.antimeridian =
        writeAntimeridianFixture(directory / "antimeridian.tif");
    paths.midSizeNoOverviews = writeMidSizeFixture(directory / "mid-size.tif");
    paths.sparseHuge = writeSparseHugeFixture(directory / "sparse-huge.tif");

    // Members are 512x512 at 2 m, placed on a grid 400 km apart, so the
    // catalog's logical extent is about 401 km across: just over 200000
    // pixels on a side and 40 billion pixels in total.
    constexpr double memberPixelSize = 2.0;
    constexpr int memberExtent = 512;
    constexpr double memberSpacing = 400000.0;
    for (int index = 0; index < 4; ++index) {
        const int row = index / 2;
        const double originX = 200000.0 + (index % 2) * memberSpacing;
        const double originY =
            7000000.0 - static_cast<double>(row) * memberSpacing;
        paths.catalogMembers.push_back(writeCatalogMember(
            directory / ("catalog-member-" + std::to_string(index) + ".tif"),
            originX,
            originY,
            memberPixelSize,
            memberExtent,
            static_cast<std::uint8_t>(40 + index * 30)));
    }
    paths.vrtMosaic =
        writeVrtMosaicFixture(directory / "mosaic.vrt", paths.catalogMembers);
    paths.catalog = writeCatalogFixture(directory / "catalog.gti.gpkg",
                                        paths.catalogMembers);
    writeCatalogOverview(paths.catalog);

    paths.mismatchedOverviews = writeMismatchedOverviewFixture(
        directory / "mismatched-overviews.vrt",
        paths.rgb,
        writeOverviewImage(directory / "red-overview.tif"));
    paths.mismatchedMaskOverviews = writeMismatchedMaskOverviewFixture(
        directory / "mismatched-mask-overviews.vrt",
        paths.rgb,
        writeOverviewImage(directory / "mask-mismatch-overview.tif"),
        writeMaskImage(directory / "mask-source.tif"));
    return paths;
}

} // namespace pci::test
