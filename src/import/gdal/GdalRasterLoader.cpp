#include "import/gdal/GdalRasterLoader.h"

#include "import/gdal/GdalSpatialReferenceComparator.h"

#include "import/gdal/GdalRasterDataset.h"
#include "import/gdal/GdalRasterSource.h"
#include "import/gdal/GdalRuntime.h"

#include <ogr_spatialref.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pci {
namespace {

// One bounded sample never exceeds this many values per selected band: 64
// windows of 64x64. Window sampling avoids turning a quarter of a million
// individual sample locations into a quarter of a million block reads.
constexpr std::size_t sampleWindowPixels = 64;
constexpr std::size_t sampleGrid = 8;
constexpr std::size_t maximumSampleValues =
    sampleWindowPixels * sampleWindowPixels * sampleGrid * sampleGrid;
constexpr double lowerPercentile = 0.02;
constexpr double upperPercentile = 0.98;

void checkCancelled(const std::stop_token &stop)
{
    if (stop.stop_requested()) {
        throw RasterImportCancelled("Raster import cancelled");
    }
}

struct BandDescriptor {
    int number = 0;
    GDALRasterBand *band = nullptr;
    GDALDataType type = GDT_Unknown;
    GDALColorInterp interpretation = GCI_Undefined;
};

[[nodiscard]] std::vector<BandDescriptor> describeBands(GDALDataset &dataset)
{
    const int count = dataset.GetRasterCount();
    if (count <= 0) {
        throw RasterImportError("Raster has no bands to display");
    }
    std::vector<BandDescriptor> bands;
    bands.reserve(static_cast<std::size_t>(count));
    for (int number = 1; number <= count; ++number) {
        GDALRasterBand *band = dataset.GetRasterBand(number);
        if (band == nullptr) {
            throw RasterImportError("Raster band " + std::to_string(number) +
                                    " could not be opened");
        }
        if (GDALDataTypeIsComplex(band->GetRasterDataType()) != 0) {
            throw RasterImportError(
                "Raster band " + std::to_string(number) +
                " has a complex data type, which is not displayable");
        }
        bands.push_back(BandDescriptor{
            .number = number,
            .band = band,
            .type = band->GetRasterDataType(),
            .interpretation = band->GetColorInterpretation(),
        });
    }
    return bands;
}

[[nodiscard]] const BandDescriptor *
findInterpretation(const std::vector<BandDescriptor> &bands,
                   const GDALColorInterp interpretation)
{
    const auto found = std::ranges::find(
        bands, interpretation, &BandDescriptor::interpretation);
    return found == bands.end() ? nullptr : &*found;
}

// A single band with no color interpretation is a measurement, not a
// photograph, so it is colorized rather than presented as grayscale. Float
// data is always treated that way: terrain is the second most common raster in
// a point-cloud workflow and is float single-band with no interpretation.
[[nodiscard]] RasterSampleKind singleBandKind(const BandDescriptor &band)
{
    if (band.type == GDT_Float32 || band.type == GDT_Float64) {
        return RasterSampleKind::ContinuousScalar;
    }
    return band.interpretation == GCI_GrayIndex
               ? RasterSampleKind::ContinuousColor
               : RasterSampleKind::ContinuousScalar;
}

[[nodiscard]] std::string
describeBandLayout(const std::vector<BandDescriptor> &bands)
{
    std::string description =
        std::to_string(bands.size()) + " bands with interpretations";
    for (const BandDescriptor &band : bands) {
        description += " ";
        description += GDALGetColorInterpretationName(band.interpretation);
    }
    return description;
}

[[nodiscard]] std::vector<std::array<float, 4>>
expandColorTable(const GDALColorTable &table)
{
    const int count = table.GetColorEntryCount();
    std::vector<std::array<float, 4>> expanded;
    expanded.reserve(static_cast<std::size_t>(std::max(0, count)));
    for (int entry = 0; entry < count; ++entry) {
        GDALColorEntry color{};
        table.GetColorEntryAsRGB(entry, &color);
        expanded.push_back({
            static_cast<float>(color.c1) / 255.0F,
            static_cast<float>(color.c2) / 255.0F,
            static_cast<float>(color.c3) / 255.0F,
            static_cast<float>(color.c4) / 255.0F,
        });
    }
    return expanded;
}

// Full-scale value of an integer band's type. Float alpha is already
// normalized to [0,1] by convention, so it scales by one.
[[nodiscard]] double dataTypeMaximum(const GDALDataType type)
{
    switch (type) {
    case GDT_Byte:
        return 255.0;
    case GDT_UInt16:
    case GDT_Int16:
        return 65535.0;
    case GDT_UInt32:
    case GDT_Int32:
        return 4294967295.0;
    default:
        return 1.0;
    }
}

void recordBandTransforms(const std::vector<BandDescriptor> &bands,
                          const std::vector<int> &selected,
                          RasterBandSelection &selection)
{
    for (const int number : selected) {
        const BandDescriptor &band =
            bands[static_cast<std::size_t>(number - 1)];
        int hasNodata = 0;
        const double nodata = band.band->GetNoDataValue(&hasNodata);
        selection.nodata.push_back(
            hasNodata != 0 ? nodata : std::numeric_limits<double>::quiet_NaN());
        selection.scale.push_back(band.band->GetScale());
        selection.offset.push_back(band.band->GetOffset());
    }
}

[[nodiscard]] RasterBandSelection selectDisplayBands(GDALDataset &dataset)
{
    const std::vector<BandDescriptor> bands = describeBands(dataset);

    RasterBandSelection selection;
    const BandDescriptor *red = findInterpretation(bands, GCI_RedBand);
    const BandDescriptor *green = findInterpretation(bands, GCI_GreenBand);
    const BandDescriptor *blue = findInterpretation(bands, GCI_BlueBand);
    const BandDescriptor *alpha = findInterpretation(bands, GCI_AlphaBand);
    const BandDescriptor *gray = findInterpretation(bands, GCI_GrayIndex);
    const BandDescriptor *palette = findInterpretation(bands, GCI_PaletteIndex);

    if (red != nullptr && green != nullptr && blue != nullptr) {
        selection.sampleKind = RasterSampleKind::ContinuousColor;
        selection.colorBands = {red->number, green->number, blue->number};
    } else if (palette != nullptr &&
               palette->band->GetColorTable() != nullptr) {
        selection.sampleKind = RasterSampleKind::Categorical;
        selection.colorBands = {palette->number};
        selection.paletteTable =
            expandColorTable(*palette->band->GetColorTable());
    } else if (gray != nullptr) {
        selection.sampleKind = singleBandKind(*gray);
        selection.colorBands = {gray->number};
    } else if (bands.size() == 1) {
        selection.sampleKind = singleBandKind(bands.front());
        selection.colorBands = {bands.front().number};
    } else if ((bands.size() == 3 || bands.size() == 4) &&
               std::ranges::all_of(bands, [&bands](const BandDescriptor &band) {
                   return band.interpretation == GCI_Undefined &&
                          band.type == bands.front().type;
               })) {
        // Positional fallback, recorded as a warning rather than presented as
        // a confident assignment.
        selection.sampleKind = RasterSampleKind::ContinuousColor;
        selection.colorBands = {
            bands[0].number, bands[1].number, bands[2].number};
        selection.positionalFallback = true;
        if (bands.size() == 4) {
            selection.alphaBand = bands[3].number;
        }
    } else {
        throw RasterImportError(
            "Raster has no identifiable RGB or grayscale band assignment: " +
            describeBandLayout(bands));
    }

    if (alpha != nullptr && selection.alphaBand == 0) {
        selection.alphaBand = alpha->number;
    }
    recordBandTransforms(bands, selection.colorBands, selection);

    selection.byteColorBands =
        std::ranges::all_of(selection.colorBands, [&bands](const int number) {
            return bands[static_cast<std::size_t>(number - 1)].type == GDT_Byte;
        });
    if (selection.alphaBand != 0) {
        const BandDescriptor &alphaBand =
            bands[static_cast<std::size_t>(selection.alphaBand - 1)];
        selection.byteAlphaBand = alphaBand.type == GDT_Byte;
        // A 16-bit alpha read as Byte would clamp almost every sample to fully
        // opaque, so its full-scale value is recorded instead.
        selection.alphaMaximum = alphaBand.type == GDT_Byte
                                     ? 255.0
                                     : dataTypeMaximum(alphaBand.type);
    }

    // A mask derived from nodata or from the alpha band adds nothing: both are
    // already composed explicitly during decode.
    const int maskFlags =
        bands[static_cast<std::size_t>(selection.colorBands.front() - 1)]
            .band->GetMaskFlags();
    selection.usesDatasetMask =
        (maskFlags & (GMF_ALL_VALID | GMF_NODATA | GMF_ALPHA)) == 0;
    return selection;
}

// Matched by dimensions, never by overview array index: an overview at index 2
// need not be an 8x reduction, and bands need not expose identical arrays.
[[nodiscard]] int matchingOverviewIndex(GDALDataset &dataset,
                                        const int bandNumber,
                                        const std::uint32_t width,
                                        const std::uint32_t height)
{
    GDALRasterBand *band = dataset.GetRasterBand(bandNumber);
    if (band == nullptr) {
        return -1;
    }
    const int count = band->GetOverviewCount();
    for (int index = 0; index < count; ++index) {
        GDALRasterBand *overview = band->GetOverview(index);
        if (overview == nullptr) {
            continue;
        }
        if (static_cast<std::uint32_t>(overview->GetXSize()) == width &&
            static_cast<std::uint32_t>(overview->GetYSize()) == height) {
            return index;
        }
    }
    return -1;
}

[[nodiscard]] std::vector<RasterLevel>
intersectBackedLevels(GDALDataset &dataset,
                      const RasterBandSelection &selection)
{
    const auto baseWidth = static_cast<std::uint32_t>(dataset.GetRasterXSize());
    const auto baseHeight =
        static_cast<std::uint32_t>(dataset.GetRasterYSize());

    const auto channels = static_cast<std::uint8_t>(
        selection.colorBands.size() + (selection.alphaBand != 0 ? 1 : 0));

    const auto makeLevel =
        [&](const std::uint32_t width,
            const std::uint32_t height,
            const int overview) -> std::optional<RasterLevel> {
        RasterLevel level;
        level.width = width;
        level.height = height;
        level.basePixelsPerTexelX =
            static_cast<double>(baseWidth) / static_cast<double>(width);
        level.basePixelsPerTexelY =
            static_cast<double>(baseHeight) / static_cast<double>(height);
        level.channelCount = channels;
        for (std::size_t index = 0; index < selection.colorBands.size();
             ++index) {
            const int band = selection.colorBands[index];
            const int matched =
                overview < 0
                    ? -1
                    : matchingOverviewIndex(dataset, band, width, height);
            if (overview >= 0 && matched < 0) {
                return std::nullopt;
            }
            level.rgbaBands[index] = RasterBandRef{band, matched};
        }
        if (selection.alphaBand != 0) {
            const int matched =
                overview < 0 ? -1
                             : matchingOverviewIndex(
                                   dataset, selection.alphaBand, width, height);
            if (overview >= 0 && matched < 0) {
                return std::nullopt;
            }
            level.rgbaBands[3] = RasterBandRef{selection.alphaBand, matched};
        }
        if (selection.usesDatasetMask) {
            // A color overview does not imply that an explicit dataset mask
            // has a matching overview. Inspect the actual selected band and
            // reject this level instead of reading a differently sized mask
            // against it.
            const RasterBandRef colorRef = level.rgbaBands[0];
            GDALRasterBand *selected = dataset.GetRasterBand(colorRef.band);
            if (selected != nullptr && colorRef.overview >= 0) {
                selected = selected->GetOverview(colorRef.overview);
            }
            GDALRasterBand *mask =
                selected == nullptr ? nullptr : selected->GetMaskBand();
            if (selected == nullptr || mask == nullptr ||
                (selected->GetMaskFlags() & GMF_ALL_VALID) != 0 ||
                static_cast<std::uint32_t>(mask->GetXSize()) != width ||
                static_cast<std::uint32_t>(mask->GetYSize()) != height) {
                return std::nullopt;
            }
            level.maskBand = colorRef;
        }
        return level;
    };

    const std::optional<RasterLevel> base =
        makeLevel(baseWidth, baseHeight, -1);
    if (!base) {
        throw RasterImportError("Raster has no readable base band");
    }
    std::vector<RasterLevel> levels{*base};

    GDALRasterBand *first = dataset.GetRasterBand(selection.colorBands.front());
    const int overviewCount = first == nullptr ? 0 : first->GetOverviewCount();
    for (int index = 0; index < overviewCount; ++index) {
        GDALRasterBand *overview = first->GetOverview(index);
        if (overview == nullptr) {
            continue;
        }
        const auto width = static_cast<std::uint32_t>(overview->GetXSize());
        const auto height = static_cast<std::uint32_t>(overview->GetYSize());
        if (width == 0 || height == 0 || width > baseWidth ||
            height > baseHeight ||
            (width == baseWidth && height == baseHeight)) {
            continue;
        }
        // A driver may omit an overview on one band. Only the dimension
        // intersection common to every required band is exposed, so no request
        // can misregister transparency against color.
        if (const std::optional<RasterLevel> level =
                makeLevel(width, height, index)) {
            levels.push_back(*level);
        }
    }

    std::sort(levels.begin() + 1,
              levels.end(),
              [](const RasterLevel &left, const RasterLevel &right) {
                  return static_cast<std::uint64_t>(left.width) * left.height >
                         static_cast<std::uint64_t>(right.width) * right.height;
              });

    std::vector<RasterLevel> ordered{levels.front()};
    for (std::size_t index = 1; index < levels.size(); ++index) {
        const RasterLevel &candidate = levels[index];
        const RasterLevel &previous = ordered.back();
        if (candidate.width > previous.width ||
            candidate.height > previous.height ||
            (candidate.width == previous.width &&
             candidate.height == previous.height)) {
            continue;
        }
        ordered.push_back(candidate);
    }
    return ordered;
}

void collectSamples(GDALRasterBand &band,
                    const int x,
                    const int y,
                    const int width,
                    const int height,
                    const double nodata,
                    const double scale,
                    const double offset,
                    std::vector<double> &samples,
                    const std::stop_token &stop,
                    std::atomic<std::uint64_t> &readCount)
{
    checkCancelled(stop);
    std::vector<double> window(static_cast<std::size_t>(width) * height, 0.0);
    readCount.fetch_add(1, std::memory_order_relaxed);
    if (band.RasterIO(GF_Read,
                      x,
                      y,
                      width,
                      height,
                      window.data(),
                      width,
                      height,
                      GDT_Float64,
                      0,
                      0,
                      nullptr) != CE_None) {
        return;
    }
    for (const double raw : window) {
        if (!std::isfinite(raw) || (std::isfinite(nodata) && raw == nodata)) {
            continue;
        }
        samples.push_back(raw * scale + offset);
    }
}

[[nodiscard]] std::optional<RasterDisplayRange>
sampleDisplayRange(GDALDataset &dataset,
                   const RasterBandSelection &selection,
                   const std::vector<RasterLevel> &levels,
                   const std::stop_token &stop,
                   std::atomic<std::uint64_t> &readCount,
                   std::optional<double> *sampledMinimum)
{
    if (sampledMinimum != nullptr) {
        sampledMinimum->reset();
    }
    // Sampling reads the coarsest usable overview, so the cost is bounded by
    // the sample budget rather than by the source dimensions.
    const RasterLevel &level = levels.back();
    const auto levelWidth = static_cast<int>(level.width);
    const auto levelHeight = static_cast<int>(level.height);
    const auto levelPixels =
        static_cast<std::size_t>(level.width) * level.height;

    std::vector<double> samples;
    for (std::size_t channel = 0; channel < selection.colorBands.size();
         ++channel) {
        GDALRasterBand *band =
            dataset.GetRasterBand(selection.colorBands[channel]);
        if (band == nullptr) {
            continue;
        }
        if (level.rgbaBands[channel].overview >= 0) {
            band = band->GetOverview(level.rgbaBands[channel].overview);
        }
        if (band == nullptr) {
            continue;
        }
        const double nodata = selection.nodata[channel];
        const double scale = selection.scale[channel];
        const double offset = selection.offset[channel];

        if (levelPixels <= maximumSampleValues) {
            collectSamples(*band,
                           0,
                           0,
                           levelWidth,
                           levelHeight,
                           nodata,
                           scale,
                           offset,
                           samples,
                           stop,
                           readCount);
            continue;
        }
        const auto window = static_cast<int>(sampleWindowPixels);
        for (std::size_t row = 0; row < sampleGrid; ++row) {
            for (std::size_t column = 0; column < sampleGrid; ++column) {
                const int spanX = std::max(0, levelWidth - window);
                const int spanY = std::max(0, levelHeight - window);
                const int x = static_cast<int>(static_cast<std::size_t>(spanX) *
                                               column / (sampleGrid - 1));
                const int y = static_cast<int>(static_cast<std::size_t>(spanY) *
                                               row / (sampleGrid - 1));
                collectSamples(*band,
                               x,
                               y,
                               std::min(window, levelWidth),
                               std::min(window, levelHeight),
                               nodata,
                               scale,
                               offset,
                               samples,
                               stop,
                               readCount);
            }
        }
    }

    if (samples.empty()) {
        return std::nullopt;
    }
    std::ranges::sort(samples);
    if (sampledMinimum != nullptr) {
        *sampledMinimum = samples.front();
    }
    const auto last = static_cast<double>(samples.size() - 1);
    const auto low =
        static_cast<std::size_t>(std::llround(last * lowerPercentile));
    const auto high =
        static_cast<std::size_t>(std::llround(last * upperPercentile));
    RasterDisplayRange range;
    range.minimum = samples[low];
    range.maximum = samples[high];
    range.origin = RasterDisplayRange::Origin::Sampled;
    if (range.minimum >= range.maximum) {
        range.minimum = samples.front();
        range.maximum = samples.back();
    }
    return range.minimum < range.maximum ? std::optional{range} : std::nullopt;
}

[[nodiscard]] bool allBandsAreByte(GDALDataset &dataset,
                                   const RasterBandSelection &selection)
{
    return std::ranges::all_of(
        selection.colorBands, [&dataset](const int band) {
            GDALRasterBand *raster = dataset.GetRasterBand(band);
            return raster != nullptr && raster->GetRasterDataType() == GDT_Byte;
        });
}

[[nodiscard]] RasterDecodeParameters
inspectOrSampleDisplayRange(GDALDataset &dataset,
                            const RasterBandSelection &selection,
                            const std::vector<RasterLevel> &levels,
                            const std::stop_token &stop,
                            std::atomic<std::uint64_t> &readCount,
                            std::optional<double> *sampledMinimum,
                            bool *samplingPerformed)
{
    if (sampledMinimum != nullptr) {
        sampledMinimum->reset();
    }
    if (samplingPerformed != nullptr) {
        *samplingPerformed = false;
    }
    RasterDecodeParameters display;
    display.sampleKind = selection.sampleKind;
    if (selection.sampleKind == RasterSampleKind::Categorical) {
        return display; // a palette index is looked up, never stretched
    }

    const bool identityScale =
        std::ranges::all_of(selection.scale,
                            [](const double value) {
                                return value == 1.0;
                            }) &&
        std::ranges::all_of(selection.offset, [](const double value) {
            return value == 0.0;
        });
    if (selection.sampleKind == RasterSampleKind::ContinuousColor &&
        identityScale && allBandsAreByte(dataset, selection)) {
        display.displayRange = RasterDisplayRange{
            .minimum = 0.0,
            .maximum = 255.0,
            .origin = RasterDisplayRange::Origin::Metadata,
        };
        return display;
    }

    // One shared range across the color bands, so a stretch cannot silently
    // change color balance.
    double low = std::numeric_limits<double>::infinity();
    double high = -std::numeric_limits<double>::infinity();
    bool cached = true;
    for (std::size_t channel = 0; channel < selection.colorBands.size();
         ++channel) {
        GDALRasterBand *band =
            dataset.GetRasterBand(selection.colorBands[channel]);
        double minimum = 0.0;
        double maximum = 0.0;
        double mean = 0.0;
        double deviation = 0.0;
        // Non-forcing: this returns cached statistics when they exist and
        // declines otherwise. It is never allowed to scan the dataset.
        if (band == nullptr ||
            band->GetStatistics(
                TRUE, FALSE, &minimum, &maximum, &mean, &deviation) !=
                CE_None) {
            cached = false;
            break;
        }
        const double scale = selection.scale[channel];
        const double offset = selection.offset[channel];
        low = std::min(low, minimum * scale + offset);
        high = std::max(high, maximum * scale + offset);
    }
    if (cached && std::isfinite(low) && std::isfinite(high) && low < high) {
        display.displayRange = RasterDisplayRange{
            .minimum = low,
            .maximum = high,
            .origin = RasterDisplayRange::Origin::CachedStatistics,
        };
        return display;
    }

    if (samplingPerformed != nullptr) {
        *samplingPerformed = true;
    }
    display.displayRange = sampleDisplayRange(
        dataset, selection, levels, stop, readCount, sampledMinimum);
    if (!display.displayRange) {
        display.displayRange = RasterDisplayRange{
            .minimum = 0.0,
            .maximum = 1.0,
            .origin = RasterDisplayRange::Origin::Metadata,
        };
    }
    return display;
}

[[nodiscard]] GDALRasterBand *referencedBand(GDALDataset &dataset,
                                             const RasterBandRef reference)
{
    GDALRasterBand *band = dataset.GetRasterBand(reference.band);
    if (band != nullptr && reference.overview >= 0) {
        band = band->GetOverview(reference.overview);
    }
    return band;
}

[[nodiscard]] std::optional<double> boundedElevationMinimum(
    GDALDataset &dataset,
    const RasterBandSelection &selection,
    const std::vector<RasterLevel> &levels,
    const std::stop_token &stop,
    std::atomic<std::uint64_t> &readCount)
{
    if (levels.empty() || selection.colorBands.size() != 1) {
        return std::nullopt;
    }
    const RasterLevel &level = levels.back();
    GDALRasterBand *band = referencedBand(dataset, level.rgbaBands[0]);
    GDALRasterBand *alpha = selection.alphaBand == 0
                                ? nullptr
                                : referencedBand(dataset, level.rgbaBands[3]);
    GDALRasterBand *maskOwner =
        level.maskBand ? referencedBand(dataset, *level.maskBand) : nullptr;
    GDALRasterBand *mask = maskOwner != nullptr ? maskOwner->GetMaskBand()
                                                : nullptr;
    if (band == nullptr || (selection.alphaBand != 0 && alpha == nullptr) ||
        (level.maskBand && mask == nullptr)) {
        return std::nullopt;
    }

    double minimum = std::numeric_limits<double>::infinity();
    const double nodata = selection.nodata.front();
    const double scale = selection.scale.front();
    const double offset = selection.offset.front();
    const auto inspectWindow = [&](const int x,
                                   const int y,
                                   const int width,
                                   const int height) {
        checkCancelled(stop);
        const std::size_t count =
            static_cast<std::size_t>(width) * height;
        std::vector<double> values(count);
        std::vector<double> alphaValues(alpha != nullptr ? count : 0);
        std::vector<unsigned char> maskValues(mask != nullptr ? count : 0);
        const auto read = [&](GDALRasterBand &source,
                              const GDALDataType type,
                              void *destination) {
            readCount.fetch_add(1, std::memory_order_relaxed);
            return source.RasterIO(GF_Read,
                                   x,
                                   y,
                                   width,
                                   height,
                                   destination,
                                   width,
                                   height,
                                   type,
                                   0,
                                   0,
                                   nullptr) == CE_None;
        };
        if (!read(*band, GDT_Float64, values.data()) ||
            (alpha != nullptr &&
             !read(*alpha, GDT_Float64, alphaValues.data())) ||
            (mask != nullptr &&
             !read(*mask, GDT_Byte, maskValues.data()))) {
            return;
        }
        for (std::size_t index = 0; index < count; ++index) {
            const double raw = values[index];
            if (!std::isfinite(raw) ||
                (std::isfinite(nodata) && raw == nodata) ||
                (alpha != nullptr &&
                 (!std::isfinite(alphaValues[index]) ||
                  alphaValues[index] <= 0.0)) ||
                (mask != nullptr && maskValues[index] == 0)) {
                continue;
            }
            const double scaled = raw * scale + offset;
            if (std::isfinite(scaled)) {
                minimum = std::min(minimum, scaled);
            }
        }
    };

    const int width = static_cast<int>(level.width);
    const int height = static_cast<int>(level.height);
    const std::size_t pixels =
        static_cast<std::size_t>(level.width) * level.height;
    if (pixels <= maximumSampleValues) {
        inspectWindow(0, 0, width, height);
    } else {
        const int window = static_cast<int>(sampleWindowPixels);
        for (std::size_t row = 0; row < sampleGrid; ++row) {
            for (std::size_t column = 0; column < sampleGrid; ++column) {
                const int x = static_cast<int>(
                    static_cast<std::size_t>(std::max(0, width - window)) *
                    column / (sampleGrid - 1));
                const int y = static_cast<int>(
                    static_cast<std::size_t>(std::max(0, height - window)) *
                    row / (sampleGrid - 1));
                inspectWindow(x,
                              y,
                              std::min(window, width),
                              std::min(window, height));
            }
        }
    }
    return std::isfinite(minimum) ? std::optional{minimum} : std::nullopt;
}

[[nodiscard]] RasterElevationDescriptor inspectElevationDescriptor(
    GDALDataset &dataset,
    const RasterBandSelection &selection,
    const std::vector<RasterLevel> &levels,
    const RasterDecodeParameters &display,
    const std::optional<double> sampledMinimum,
    const bool samplingPerformed,
    const std::stop_token &stop,
    std::atomic<std::uint64_t> &readCount)
{
    RasterElevationDescriptor descriptor;
    if (selection.sampleKind != RasterSampleKind::ContinuousScalar ||
        selection.colorBands.size() != 1) {
        return descriptor;
    }
    GDALRasterBand *band = dataset.GetRasterBand(selection.colorBands.front());
    if (band == nullptr) {
        return descriptor;
    }
    descriptor.band = selection.colorBands.front();
    descriptor.scale = selection.scale.front();
    descriptor.offset = selection.offset.front();
    if (const char *unit = band->GetUnitType(); unit != nullptr) {
        descriptor.unit = unit;
    }

    double rawMinimum = 0.0;
    double rawMaximum = 0.0;
    double mean = 0.0;
    double deviation = 0.0;
    const char *approximate = band->GetMetadataItem("STATISTICS_APPROXIMATE");
    const bool exactStatistics =
        selection.alphaBand == 0 && !selection.usesDatasetMask &&
        (approximate == nullptr || !CPLTestBool(approximate)) &&
        band->GetStatistics(FALSE,
                            FALSE,
                            &rawMinimum,
                            &rawMaximum,
                            &mean,
                            &deviation) == CE_None &&
        std::isfinite(rawMinimum) && std::isfinite(rawMaximum);
    if (exactStatistics) {
        const double first =
            rawMinimum * descriptor.scale + descriptor.offset;
        const double second =
            rawMaximum * descriptor.scale + descriptor.offset;
        if (std::isfinite(first) && std::isfinite(second)) {
            descriptor.available = true;
            descriptor.cachedExactRange = RasterElevationRange{
                .minimum = std::min(first, second),
                .maximum = std::max(first, second),
            };
            descriptor.anchor = std::floor(descriptor.cachedExactRange->minimum);
            return descriptor;
        }
    }

    // The scalar display-range pass has already performed the same bounded
    // sampling when there is no separate validity source. Reuse its scaled
    // lower estimate as the precision anchor instead of doubling import I/O.
    // Alpha and dataset-mask sources cannot take this shortcut because the
    // display sampler intentionally does not compose their validity.
    if (selection.alphaBand == 0 && !selection.usesDatasetMask) {
        if (samplingPerformed) {
            if (sampledMinimum && std::isfinite(*sampledMinimum)) {
                descriptor.available = true;
                descriptor.anchor = std::floor(*sampledMinimum);
            }
            // The completed scalar sample also proves that no valid value was
            // found when sampledMinimum is empty. A second identical pass
            // cannot add information.
            return descriptor;
        }
        if (display.displayRange.has_value() &&
            display.displayRange->origin !=
                RasterDisplayRange::Origin::Metadata &&
            std::isfinite(display.displayRange->minimum)) {
            descriptor.available = true;
            descriptor.anchor = std::floor(display.displayRange->minimum);
            return descriptor;
        }
    }

    if (const std::optional<double> minimum = boundedElevationMinimum(
            dataset, selection, levels, stop, readCount)) {
        descriptor.available = true;
        descriptor.anchor = std::floor(*minimum);
    }
    return descriptor;
}

[[nodiscard]] std::string spatialReferenceWkt(GDALDataset &dataset,
                                              bool &geographic)
{
    geographic = false;
    const OGRSpatialReference *reference = dataset.GetSpatialRef();
    if (reference == nullptr) {
        return {};
    }
    geographic = reference->IsGeographic() != 0;
    char *wkt = nullptr;
    if (reference->exportToWkt(&wkt) != OGRERR_NONE || wkt == nullptr) {
        CPLFree(wkt);
        return {};
    }
    std::string exported(wkt);
    CPLFree(wkt);
    return exported;
}

[[nodiscard]] bool crsDiffers(const std::string &source,
                              const std::string &target)
{
    return GdalSpatialReferenceComparator{}.compare(source, target) ==
           SpatialReferenceRelation::Different;
}

[[nodiscard]] bool xyDisjoint(const Bounds3d &left, const Bounds3d &right)
{
    return left.maximum[0] < right.minimum[0] ||
           right.maximum[0] < left.minimum[0] ||
           left.maximum[1] < right.minimum[1] ||
           right.maximum[1] < left.minimum[1];
}

[[nodiscard]] std::vector<RasterBandInfo> describeBandInfo(GDALDataset &dataset)
{
    std::vector<RasterBandInfo> described;
    const int count = dataset.GetRasterCount();
    described.reserve(static_cast<std::size_t>(std::max(0, count)));
    for (int number = 1; number <= count; ++number) {
        GDALRasterBand *band = dataset.GetRasterBand(number);
        if (band == nullptr) {
            continue;
        }
        int hasNodata = 0;
        const double nodata = band->GetNoDataValue(&hasNodata);
        const char *description = band->GetDescription();
        described.push_back(RasterBandInfo{
            .band = number,
            .name = description == nullptr ? std::string() : description,
            .dataType = GDALGetDataTypeName(band->GetRasterDataType()),
            .colorInterpretation =
                GDALGetColorInterpretationName(band->GetColorInterpretation()),
            .nodata = hasNodata != 0 ? std::optional{nodata} : std::nullopt,
        });
    }
    return described;
}

} // namespace

RasterImportPreflight
GdalRasterLoader::inspect(const RasterImportRequest &request) const
{
    checkCancelled(request.stopToken);
    const GdalDatasetPtr dataset = openRasterDataset(request.sourcePath);

    const int width = dataset->GetRasterXSize();
    const int height = dataset->GetRasterYSize();
    if (width <= 0 || height <= 0) {
        throw RasterImportError("Raster has no positive pixel dimensions");
    }

    std::array<double, 6> geoTransform{};
    // A missing CRS is a warning, but a missing geotransformation makes
    // placement impossible and is therefore an import error.
    if (dataset->GetGeoTransform(geoTransform.data()) != CE_None ||
        !rasterAffineInvertible(geoTransform)) {
        throw RasterImportError("Raster has no usable affine geotransform");
    }

    checkCancelled(request.stopToken);
    if (request.phase) {
        request.phase(RasterImportPhase::Inspecting);
    }
    RasterBandSelection selection = selectDisplayBands(*dataset);
    std::vector<RasterLevel> levels =
        intersectBackedLevels(*dataset, selection);

    RasterLayerMetadata metadata;
    metadata.sourcePath = request.sourcePath;
    if (GDALDriver *driver = dataset->GetDriver(); driver != nullptr) {
        metadata.sourceDriver = driver->GetDescription();
    }
    metadata.width = static_cast<std::uint32_t>(width);
    metadata.height = static_cast<std::uint32_t>(height);
    metadata.geoTransform = geoTransform;
    metadata.nativePixelSize = {
        std::hypot(geoTransform[1], geoTransform[4]),
        std::hypot(geoTransform[2], geoTransform[5]),
    };
    metadata.positionalBandFallback = selection.positionalFallback;
    metadata.spatialReferenceWkt =
        spatialReferenceWkt(*dataset, metadata.geographicCrs);
    metadata.crsMissing = metadata.spatialReferenceWkt.empty();
    metadata.bands = describeBandInfo(*dataset);
    metadata.levels = std::move(levels);

    const std::optional<Bounds3d> bounds =
        rasterPixelEdgeBounds(geoTransform, metadata.width, metadata.height);
    if (!bounds) {
        throw RasterImportError("Raster corners are not finite");
    }
    metadata.bounds = *bounds;

    // A wrapped bound is not confined to the offending layer: it corrupts
    // scene fitting and framing for the whole document, so the import fails
    // rather than admitting a layer with world-spanning bounds.
    metadata.crossesAntimeridian =
        rasterCrossesAntimeridian(metadata.bounds, metadata.geographicCrs);
    if (metadata.crossesAntimeridian) {
        throw RasterImportError(
            "Raster crosses the antimeridian and cannot be placed without "
            "reprojection");
    }

    metadata.crsMismatch = crsDiffers(metadata.spatialReferenceWkt,
                                      request.targetSpatialReferenceWkt);
    metadata.extentDisjointXY =
        request.targetExtent.has_value() &&
        xyDisjoint(metadata.bounds, *request.targetExtent);
    metadata.insufficientOverviews = rasterRequiresTiledRendering(metadata);

    checkCancelled(request.stopToken);
    if (request.phase) {
        request.phase(RasterImportPhase::SamplingRange);
    }
    std::optional<double> sampledElevationMinimum;
    bool displaySamplingPerformed = false;
    metadata.defaultDisplay = inspectOrSampleDisplayRange(
        *dataset,
        selection,
        metadata.levels,
        request.stopToken,
        sampleReadCount_,
        &sampledElevationMinimum,
        &displaySamplingPerformed);
    metadata.elevation = inspectElevationDescriptor(*dataset,
                                                    selection,
                                                    metadata.levels,
                                                    metadata.defaultDisplay,
                                                    sampledElevationMinimum,
                                                    displaySamplingPerformed,
                                                    request.stopToken,
                                                    sampleReadCount_);

    // Keep inspection and range sampling on physically backed GDAL levels.
    // The logical coverage pyramid is metadata-only here; its pixels are read
    // asynchronously by the normal tile streamer when the viewport needs them.
    appendGeneratedRasterCoverageLevels(
        metadata.levels, metadata.width, metadata.height);

    if (!rasterLevelTableValid(
            metadata.levels, metadata.width, metadata.height)) {
        throw RasterImportError("Raster has no common readable band level");
    }

    auto source = std::make_shared<GdalRasterSource>(
        request.sourcePath, metadata, std::move(selection));
    auto data = std::make_shared<RasterLayerData>(RasterLayerData{
        .sourceId = nextRasterSourceId(),
        .source = std::move(source),
    });
    return RasterImportPreflight{.data = std::move(data)};
}

} // namespace pci
