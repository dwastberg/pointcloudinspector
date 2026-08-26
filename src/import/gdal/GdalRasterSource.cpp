#include "import/gdal/GdalRasterSource.h"

#include "foundation/CheckedArithmetic.h"
#include "import/gdal/GdalRasterDataset.h"
#include "import/gdal/GdalRuntime.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace pci {
namespace {

// GDAL calls the progress function during long reads. Returning false aborts
// the RasterIO, but not every driver calls it often, so cancellation is also
// checked between band reads.
int abortWhenStopRequested(double, const char *, void *data)
{
    const auto *stop = static_cast<const std::stop_token *>(data);
    return stop != nullptr && stop->stop_requested() ? FALSE : TRUE;
}

void checkCancelled(const std::stop_token &stop)
{
    if (stop.stop_requested()) {
        throw RasterReadCancelled{};
    }
}

struct TileWindow {
    int readX = 0;
    int readY = 0;
    int readWidth = 0;
    int readHeight = 0;
    // Offset of the read region inside the stored guttered buffer. It is
    // nonzero exactly when the requested gutter fell outside the level.
    int destX = 0;
    int destY = 0;
    std::uint32_t validWidth = 0;
    std::uint32_t validHeight = 0;
};

[[nodiscard]] TileWindow tileWindow(const RasterLevel &level,
                                    const RasterTileKey key)
{
    const RasterTileExtent extent = rasterTileValidExtent(level, key);
    if (extent.width == 0 || extent.height == 0) {
        throw RasterReadError("Raster tile key is outside its level");
    }

    const auto gutter = static_cast<int>(rasterTileGutter);
    const auto originX = static_cast<int>(key.x * rasterTilePixels);
    const auto originY = static_cast<int>(key.y * rasterTilePixels);
    const int desiredX = originX - gutter;
    const int desiredY = originY - gutter;

    TileWindow window;
    window.readX = std::max(0, desiredX);
    window.readY = std::max(0, desiredY);
    // At the dataset border the gutter falls outside valid pixels. Clamping
    // the window and replicating the edge texel later keeps GDAL from erroring
    // or returning fill.
    const int right =
        std::min(static_cast<int>(level.width),
                 originX + static_cast<int>(extent.width) + gutter);
    const int bottom =
        std::min(static_cast<int>(level.height),
                 originY + static_cast<int>(extent.height) + gutter);
    window.readWidth = right - window.readX;
    window.readHeight = bottom - window.readY;
    window.destX = window.readX - desiredX;
    window.destY = window.readY - desiredY;
    window.validWidth = extent.width;
    window.validHeight = extent.height;
    return window;
}

// Samples are held as bytes when every selected band is Byte, and as doubles
// otherwise. A 4096x4096 static read of three Byte bands is 50 MB this way and
// 400 MB if everything were widened to double.
struct ChannelPlanes {
    bool byteSamples = false;
    std::size_t pixelCount = 0;
    std::vector<std::vector<unsigned char>> colorBytes;
    std::vector<std::vector<double>> colorDoubles;
    std::vector<unsigned char> alphaBytes;
    std::vector<double> alphaDoubles;
    std::vector<unsigned char> mask;

    [[nodiscard]] std::size_t channelCount() const noexcept
    {
        return byteSamples ? colorBytes.size() : colorDoubles.size();
    }

    [[nodiscard]] double color(const std::size_t channel,
                               const std::size_t index) const noexcept
    {
        return byteSamples ? static_cast<double>(colorBytes[channel][index])
                           : colorDoubles[channel][index];
    }

    [[nodiscard]] bool hasAlpha() const noexcept
    {
        return !alphaBytes.empty() || !alphaDoubles.empty();
    }

    [[nodiscard]] double alpha(const std::size_t index) const noexcept
    {
        return alphaBytes.empty() ? alphaDoubles[index]
                                  : static_cast<double>(alphaBytes[index]);
    }
};

// A rectangular read: window dimensions in the source band, buffer dimensions
// in the destination. They are equal for a backed tile and differ for a
// generated coverage tile.
struct ReadExtent {
    int sourceX = 0;
    int sourceY = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int bufferWidth = 0;
    int bufferHeight = 0;
    bool floatingWindow = false;
    double floatingX = 0.0;
    double floatingY = 0.0;
    double floatingWidth = 0.0;
    double floatingHeight = 0.0;
};

[[nodiscard]] GDALRasterBand *levelBand(GDALDataset &dataset,
                                        const RasterBandRef &reference);

[[nodiscard]] ReadExtent readExtentFor(GDALDataset &dataset,
                                       const RasterLevel &level,
                                       const TileWindow &window)
{
    ReadExtent extent{
        .sourceX = window.readX,
        .sourceY = window.readY,
        .sourceWidth = window.readWidth,
        .sourceHeight = window.readHeight,
        .bufferWidth = window.readWidth,
        .bufferHeight = window.readHeight,
    };
    if (level.kind != RasterLevelKind::GeneratedCoverage) {
        return extent;
    }

    GDALRasterBand *backing = levelBand(dataset, level.rgbaBands[0]);
    if (backing == nullptr || level.width == 0 || level.height == 0) {
        throw RasterReadError("Generated raster coverage has no backing band");
    }
    const double scaleX =
        static_cast<double>(backing->GetXSize()) / level.width;
    const double scaleY =
        static_cast<double>(backing->GetYSize()) / level.height;
    extent.floatingWindow = true;
    extent.floatingX = static_cast<double>(window.readX) * scaleX;
    extent.floatingY = static_cast<double>(window.readY) * scaleY;
    extent.floatingWidth = static_cast<double>(window.readWidth) * scaleX;
    extent.floatingHeight = static_cast<double>(window.readHeight) * scaleY;
    extent.sourceX = std::clamp(static_cast<int>(std::floor(extent.floatingX)),
                                0,
                                backing->GetXSize() - 1);
    extent.sourceY = std::clamp(static_cast<int>(std::floor(extent.floatingY)),
                                0,
                                backing->GetYSize() - 1);
    const int sourceRight = std::clamp(
        static_cast<int>(std::ceil(extent.floatingX + extent.floatingWidth)),
        extent.sourceX + 1,
        backing->GetXSize());
    const int sourceBottom = std::clamp(
        static_cast<int>(std::ceil(extent.floatingY + extent.floatingHeight)),
        extent.sourceY + 1,
        backing->GetYSize());
    extent.sourceWidth = sourceRight - extent.sourceX;
    extent.sourceHeight = sourceBottom - extent.sourceY;
    return extent;
}

[[nodiscard]] GDALRasterBand *levelBand(GDALDataset &dataset,
                                        const RasterBandRef &reference)
{
    GDALRasterBand *band = dataset.GetRasterBand(reference.band);
    if (band == nullptr) {
        throw RasterReadError("Raster band is unavailable");
    }
    if (reference.overview < 0) {
        return band;
    }
    GDALRasterBand *overview = band->GetOverview(reference.overview);
    if (overview == nullptr) {
        throw RasterReadError("Raster overview band is unavailable");
    }
    return overview;
}

template <typename Sample>
void readExtent(GDALRasterBand &band,
                const ReadExtent &extent,
                const GDALDataType type,
                std::vector<Sample> &destination,
                const std::stop_token &stop,
                std::atomic<std::uint64_t> &readCount,
                const GDALRIOResampleAlg resampling = GRIORA_NearestNeighbour)
{
    destination.assign(static_cast<std::size_t>(extent.bufferWidth) *
                           extent.bufferHeight,
                       Sample{});

    const auto perform = [&](const GDALRIOResampleAlg algorithm) {
        GDALRasterIOExtraArg extra;
        INIT_RASTERIO_EXTRA_ARG(extra);
        extra.eResampleAlg = algorithm;
        extra.pfnProgress = abortWhenStopRequested;
        extra.pProgressData = const_cast<std::stop_token *>(&stop);
        if (extent.floatingWindow) {
            extra.bFloatingPointWindowValidity = TRUE;
            extra.dfXOff = extent.floatingX;
            extra.dfYOff = extent.floatingY;
            extra.dfXSize = extent.floatingWidth;
            extra.dfYSize = extent.floatingHeight;
        }
        readCount.fetch_add(1, std::memory_order_relaxed);
        return band.RasterIO(GF_Read,
                             extent.sourceX,
                             extent.sourceY,
                             extent.sourceWidth,
                             extent.sourceHeight,
                             destination.data(),
                             extent.bufferWidth,
                             extent.bufferHeight,
                             type,
                             0,
                             0,
                             &extra);
    };

    CPLErr status = perform(resampling);
    if (status != CE_None && resampling != GRIORA_NearestNeighbour) {
        checkCancelled(stop);
        status = perform(GRIORA_NearestNeighbour);
    }
    checkCancelled(stop);
    if (status != CE_None) {
        throw RasterReadError("Raster window read failed");
    }
}

// Reads every required band of one level into typed planes.
ChannelPlanes readPlanes(GDALDataset &dataset,
                         const RasterLevel &level,
                         const RasterBandSelection &selection,
                         const ReadExtent &extent,
                         const std::stop_token &stop,
                         std::atomic<std::uint64_t> &readCount)
{
    ChannelPlanes planes;
    planes.byteSamples = selection.byteColorBands;
    planes.pixelCount =
        static_cast<std::size_t>(extent.bufferWidth) * extent.bufferHeight;

    const std::size_t channels = selection.colorBands.size();
    const bool preserveExactNodata =
        std::ranges::any_of(selection.nodata, [](const double value) {
            return std::isfinite(value);
        });
    // Filtering raw color separately from validity can blend an invalid
    // source color into a valid output texel. Keep nodata, alpha, masks, and
    // categorical values exact; ordinary continuous imagery uses bilinear.
    const GDALRIOResampleAlg colorResampling =
        level.kind == RasterLevelKind::GeneratedCoverage &&
                selection.sampleKind != RasterSampleKind::Categorical &&
                !preserveExactNodata && selection.alphaBand == 0 &&
                !level.maskBand
            ? GRIORA_Bilinear
            : GRIORA_NearestNeighbour;
    if (planes.byteSamples) {
        planes.colorBytes.resize(channels);
    } else {
        planes.colorDoubles.resize(channels);
    }
    for (std::size_t channel = 0; channel < channels; ++channel) {
        checkCancelled(stop);
        GDALRasterBand &band = *levelBand(dataset, level.rgbaBands[channel]);
        if (planes.byteSamples) {
            readExtent(band,
                       extent,
                       GDT_Byte,
                       planes.colorBytes[channel],
                       stop,
                       readCount,
                       colorResampling);
        } else {
            readExtent(band,
                       extent,
                       GDT_Float64,
                       planes.colorDoubles[channel],
                       stop,
                       readCount,
                       colorResampling);
        }
    }

    if (selection.alphaBand != 0) {
        checkCancelled(stop);
        GDALRasterBand &band = *levelBand(dataset, level.rgbaBands[3]);
        if (selection.byteAlphaBand) {
            readExtent(band,
                       extent,
                       GDT_Byte,
                       planes.alphaBytes,
                       stop,
                       readCount,
                       colorResampling);
        } else {
            readExtent(band,
                       extent,
                       GDT_Float64,
                       planes.alphaDoubles,
                       stop,
                       readCount,
                       colorResampling);
        }
    }

    if (level.maskBand) {
        checkCancelled(stop);
        // The mask comes from the same overview band as the color, so its
        // dimensions match by construction rather than by a separate
        // intersection a driver could violate.
        GDALRasterBand *mask =
            levelBand(dataset, *level.maskBand)->GetMaskBand();
        if (mask == nullptr) {
            throw RasterReadError("Raster mask band is unavailable");
        }
        readExtent(*mask,
                   extent,
                   GDT_Byte,
                   planes.mask,
                   stop,
                   readCount,
                   GRIORA_NearestNeighbour);
    }
    return planes;
}

[[nodiscard]] std::array<float, 4> toRgba(const PointRgba &color) noexcept
{
    return {color.red, color.green, color.blue, color.alpha};
}

[[nodiscard]] std::array<float, 4>
sampleRamp(const std::vector<PointColorStop> &stops, const double position)
{
    if (stops.empty()) {
        const auto gray = static_cast<float>(position);
        return {gray, gray, gray, 1.0F};
    }
    if (position <= stops.front().position) {
        return toRgba(stops.front().color);
    }
    if (position >= stops.back().position) {
        return toRgba(stops.back().color);
    }
    for (std::size_t index = 1; index < stops.size(); ++index) {
        if (position > stops[index].position) {
            continue;
        }
        const PointColorStop &low = stops[index - 1];
        const PointColorStop &high = stops[index];
        const double span = static_cast<double>(high.position) - low.position;
        const double blend =
            span > 0.0 ? (position - low.position) / span : 0.0;
        const std::array<float, 4> a = toRgba(low.color);
        const std::array<float, 4> b = toRgba(high.color);
        std::array<float, 4> mixed{};
        for (std::size_t channel = 0; channel < 4; ++channel) {
            mixed[channel] = static_cast<float>(
                a[channel] + (b[channel] - a[channel]) * blend);
        }
        return mixed;
    }
    return toRgba(stops.back().color);
}

[[nodiscard]] std::byte quantize(const double value) noexcept
{
    const double clamped = std::clamp(value, 0.0, 1.0);
    return static_cast<std::byte>(std::lround(clamped * 255.0));
}

[[nodiscard]] bool matchesNodata(const double sample,
                                 const double nodata) noexcept
{
    return std::isfinite(nodata) && sample == nodata;
}

// Destination geometry for one decode: where the read buffer lands inside the
// output image, and how large that output is.
struct DecodeTarget {
    int width = 0;
    int height = 0;
    int offsetX = 0;
    int offsetY = 0;
};

struct ExpandedTile {
    std::vector<std::byte> rgba;
    std::vector<float> elevation;
    float elevationMinimum = 0.0F;
    float elevationMaximum = 0.0F;
    bool hasValidElevation = false;
    bool hasTranslucentAlpha = false;
};

ExpandedTile
expandPremultipliedRgba(const ChannelPlanes &planes,
                        const ReadExtent &extent,
                        const DecodeTarget &target,
                        const RasterBandSelection &selection,
                        const RasterDecodeParameters &decode,
                        const RasterTilePayloadProfile profile,
                        const double elevationAnchor)
{
    ExpandedTile result;
    const std::size_t pixelCount =
        static_cast<std::size_t>(target.width) * target.height;
    result.rgba.assign(pixelCount * 4, std::byte{});
    if (profile == RasterTilePayloadProfile::RenderElevation) {
        result.elevation.assign(
            pixelCount, std::numeric_limits<float>::quiet_NaN());
        result.elevationMinimum = std::numeric_limits<float>::infinity();
        result.elevationMaximum = -std::numeric_limits<float>::infinity();
    }

    double low = 0.0;
    double high = 255.0;
    if (decode.displayRange) {
        low = decode.displayRange->minimum;
        high = decode.displayRange->maximum;
    }
    const double span = high - low;
    const double inverseSpan = std::abs(span) > 0.0 ? 1.0 / span : 0.0;
    const auto normalize = [low, inverseSpan](const double sample) {
        return std::clamp((sample - low) * inverseSpan, 0.0, 1.0);
    };

    const std::vector<PointColorStop> *ramp =
        decode.colorRamp ? decode.colorRamp.get() : nullptr;
    const std::size_t channels = planes.channelCount();
    const double alphaScale =
        selection.alphaMaximum > 0.0 ? 1.0 / selection.alphaMaximum : 0.0;

    for (int y = 0; y < target.height; ++y) {
        // Clamping the source coordinate replicates the edge texel into any
        // gutter that fell outside the level, in the same pass as the expand.
        const int sourceY =
            std::clamp(y - target.offsetY, 0, extent.bufferHeight - 1);
        for (int x = 0; x < target.width; ++x) {
            const int sourceX =
                std::clamp(x - target.offsetX, 0, extent.bufferWidth - 1);
            const auto sourceIndex =
                static_cast<std::size_t>(sourceY) * extent.bufferWidth +
                sourceX;

            bool valid = true;
            std::array<float, 4> color{0.0F, 0.0F, 0.0F, 1.0F};
            std::array<double, 3> scaled{};
            for (std::size_t channel = 0; channel < channels; ++channel) {
                const double sample = planes.color(channel, sourceIndex);
                // Nodata is declared in raw units, so it is compared before
                // scale and offset move the sample into the display domain.
                if (!std::isfinite(sample) ||
                    matchesNodata(sample, selection.nodata[channel])) {
                    valid = false;
                }
                if (channel < scaled.size()) {
                    scaled[channel] = sample * selection.scale[channel] +
                                      selection.offset[channel];
                }
            }

            const double first = scaled[0];
            switch (selection.sampleKind) {
            case RasterSampleKind::ContinuousColor:
                if (channels >= 3) {
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        color[channel] =
                            static_cast<float>(normalize(scaled[channel]));
                    }
                } else {
                    const auto gray = static_cast<float>(normalize(first));
                    color = {gray, gray, gray, 1.0F};
                }
                break;
            case RasterSampleKind::ContinuousScalar: {
                const auto position = static_cast<float>(normalize(first));
                color = ramp == nullptr ? std::array<float, 4>{position,
                                                               position,
                                                               position,
                                                               1.0F}
                                        : sampleRamp(*ramp, normalize(first));
                break;
            }
            case RasterSampleKind::Categorical: {
                const long rounded = std::lround(first);
                const auto entry =
                    static_cast<std::size_t>(std::max(0L, rounded));
                color = entry < selection.paletteTable.size()
                            ? selection.paletteTable[entry]
                            : std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F};
                break;
            }
            }

            // colorTableAlpha * explicitAlpha * validity, composed before
            // premultiplication so the GPU's linear filter never blends an
            // invalid source color into a valid neighbour.
            double sourceAlpha = 1.0;
            if (planes.hasAlpha()) {
                const double sample = planes.alpha(sourceIndex);
                sourceAlpha = std::isfinite(sample)
                                  ? std::clamp(sample * alphaScale, 0.0, 1.0)
                                  : 0.0;
            }
            bool maskValid = true;
            if (!planes.mask.empty()) {
                maskValid = planes.mask[sourceIndex] > 0;
            }
            const bool heightValid = valid && maskValid && sourceAlpha > 0.0 &&
                                     std::isfinite(first);

            double alpha = static_cast<double>(color[3]) * sourceAlpha *
                           (maskValid && valid ? 1.0 : 0.0);
            if (!std::isfinite(alpha)) {
                alpha = 0.0;
            }

            const auto destination =
                (static_cast<std::size_t>(y) * target.width + x) * 4;
            result.rgba[destination] = quantize(color[0] * alpha);
            result.rgba[destination + 1] = quantize(color[1] * alpha);
            result.rgba[destination + 2] = quantize(color[2] * alpha);
            const std::byte storedAlpha = quantize(alpha);
            result.rgba[destination + 3] = storedAlpha;
            const unsigned int alphaByte = std::to_integer<unsigned int>(
                storedAlpha);
            result.hasTranslucentAlpha =
                result.hasTranslucentAlpha ||
                (alphaByte > 0U && alphaByte < 255U);

            if (profile == RasterTilePayloadProfile::RenderElevation &&
                heightValid) {
                const float residual =
                    static_cast<float>(first - elevationAnchor);
                if (std::isfinite(residual)) {
                    const std::size_t heightIndex =
                        static_cast<std::size_t>(y) * target.width + x;
                    result.elevation[heightIndex] = residual;
                    result.elevationMinimum =
                        std::min(result.elevationMinimum, residual);
                    result.elevationMaximum =
                        std::max(result.elevationMaximum, residual);
                    result.hasValidElevation = true;
                }
            }
        }
    }
    if (!result.elevation.empty()) {
        const float fill =
            result.hasValidElevation ? result.elevationMinimum : 0.0F;
        for (float &height : result.elevation) {
            if (!std::isfinite(height)) {
                height = fill;
            }
        }
        if (!result.hasValidElevation) {
            result.elevationMinimum = 0.0F;
            result.elevationMaximum = 0.0F;
        }
    }
    return result;
}

} // namespace

// At most one exclusively leased handle per worker, because a GDAL dataset
// handle is never safe to use concurrently. Each additional handle to a VRT or
// GTI dataset opens its own member datasets, which is why the cap is low.
struct GdalRasterHandlePool {
    explicit GdalRasterHandlePool(std::filesystem::path path,
                                  const std::uint32_t maximum)
        : sourcePath(std::move(path))
        , maximumHandles(std::max<std::uint32_t>(1, maximum))
    {
    }

    std::filesystem::path sourcePath;
    std::uint32_t maximumHandles;
    std::mutex mutex;
    std::condition_variable_any available;
    std::vector<GdalDatasetPtr> idle;
    std::uint32_t openHandles = 0;
};

namespace {

// Returns a handle to the pool on scope exit even when the read throws.
class HandleLease final {
public:
    HandleLease(GdalRasterHandlePool &pool, GdalDatasetPtr dataset)
        : pool_(&pool)
        , dataset_(std::move(dataset))
    {
    }

    ~HandleLease()
    {
        if (dataset_ == nullptr) {
            return;
        }
        {
            const std::lock_guard<std::mutex> guard(pool_->mutex);
            pool_->idle.push_back(std::move(dataset_));
        }
        pool_->available.notify_one();
    }

    HandleLease(const HandleLease &) = delete;
    HandleLease &operator=(const HandleLease &) = delete;
    HandleLease(HandleLease &&) = delete;
    HandleLease &operator=(HandleLease &&) = delete;

    [[nodiscard]] GDALDataset &dataset() const noexcept
    {
        return *dataset_;
    }

private:
    GdalRasterHandlePool *pool_;
    GdalDatasetPtr dataset_;
};

[[nodiscard]] GdalDatasetPtr acquireHandle(GdalRasterHandlePool &pool,
                                           const std::stop_token &stop)
{
    std::unique_lock<std::mutex> guard(pool.mutex);
    for (;;) {
        checkCancelled(stop);
        if (!pool.idle.empty()) {
            GdalDatasetPtr dataset = std::move(pool.idle.back());
            pool.idle.pop_back();
            return dataset;
        }
        if (pool.openHandles < pool.maximumHandles) {
            ++pool.openHandles;
            guard.unlock();
            try {
                return openRasterDataset(pool.sourcePath);
            } catch (...) {
                const std::lock_guard<std::mutex> failure(pool.mutex);
                --pool.openHandles;
                throw;
            }
        }
        if (!pool.available.wait(guard, stop, [&pool] {
                return !pool.idle.empty();
            })) {
            throw RasterReadCancelled{};
        }
    }
}

} // namespace

GdalRasterSource::GdalRasterSource(std::filesystem::path sourcePath,
                                   RasterLayerMetadata metadata,
                                   RasterBandSelection selection,
                                   const std::uint32_t maximumHandles)
    : sourcePath_(std::move(sourcePath))
    , metadata_(std::move(metadata))
    , selection_(std::move(selection))
    , handles_(
          std::make_unique<GdalRasterHandlePool>(sourcePath_, maximumHandles))
{
    if (selection_.colorBands.empty()) {
        throw RasterImportError("Raster source has no selected color band");
    }
    const std::size_t channels = selection_.colorBands.size();
    if (selection_.nodata.size() != channels) {
        selection_.nodata.assign(channels,
                                 std::numeric_limits<double>::quiet_NaN());
    }
    if (selection_.scale.size() != channels) {
        selection_.scale.assign(channels, 1.0);
    }
    if (selection_.offset.size() != channels) {
        selection_.offset.assign(channels, 0.0);
    }
}

GdalRasterSource::~GdalRasterSource() = default;

const RasterLayerMetadata &GdalRasterSource::metadata() const noexcept
{
    return metadata_;
}

RasterTileSourcePtr
GdalRasterSource::detachedReader(const std::uint32_t maximumHandles) const
{
    return std::make_shared<GdalRasterSource>(
        sourcePath_, metadata_, selection_, maximumHandles);
}

std::uint64_t GdalRasterSource::readCount() const noexcept
{
    return readCount_.load(std::memory_order_relaxed);
}

std::uint64_t
GdalRasterSource::exactElevationScanReservationBytes() const noexcept
{
    constexpr std::uint64_t scanPixels = 256ULL * 256ULL;
    // Scalar and optional alpha are widened to double; the dataset mask stays
    // byte-sized. The same allocations are reused for every scan block.
    return sizeof(RasterElevationRange) +
           scanPixels * (2ULL * sizeof(double) + sizeof(unsigned char));
}

RasterElevationRange GdalRasterSource::exactElevationRange(
    const std::stop_token stop,
    RasterElevationProgressCallback progress) const
{
    if (!metadata_.elevation.available ||
        selection_.sampleKind != RasterSampleKind::ContinuousScalar ||
        selection_.colorBands.size() != 1) {
        throw RasterReadError("Raster is not an elevation source");
    }

    checkCancelled(stop);
    const HandleLease lease(*handles_, acquireHandle(*handles_, stop));
    GDALRasterBand *band =
        lease.dataset().GetRasterBand(selection_.colorBands.front());
    GDALRasterBand *alpha = selection_.alphaBand == 0
                                ? nullptr
                                : lease.dataset().GetRasterBand(
                                      selection_.alphaBand);
    GDALRasterBand *mask =
        selection_.usesDatasetMask && band != nullptr ? band->GetMaskBand()
                                                      : nullptr;
    if (band == nullptr || (selection_.alphaBand != 0 && alpha == nullptr) ||
        (selection_.usesDatasetMask && mask == nullptr)) {
        throw RasterReadError("Raster elevation bands are unavailable");
    }

    constexpr int scanBlock = 256;
    const int width = band->GetXSize();
    const int height = band->GetYSize();
    const std::uint64_t blocksX =
        static_cast<std::uint64_t>((width + scanBlock - 1) / scanBlock);
    const std::uint64_t blocksY =
        static_cast<std::uint64_t>((height + scanBlock - 1) / scanBlock);
    const std::uint64_t total = blocksX * blocksY;
    std::uint64_t processed = 0;
    if (progress) {
        progress({.processedBlocks = 0, .totalBlocks = total});
    }

    std::vector<double> values;
    std::vector<double> alphaValues;
    std::vector<unsigned char> maskValues;
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    const double nodata = selection_.nodata.front();
    const double scale = metadata_.elevation.scale;
    const double offset = metadata_.elevation.offset;

    const auto read = [&](GDALRasterBand &source,
                          const int x,
                          const int y,
                          const int blockWidth,
                          const int blockHeight,
                          const GDALDataType type,
                          void *destination) {
        GDALRasterIOExtraArg extra;
        INIT_RASTERIO_EXTRA_ARG(extra);
        extra.pfnProgress = abortWhenStopRequested;
        extra.pProgressData = const_cast<std::stop_token *>(&stop);
        readCount_.fetch_add(1, std::memory_order_relaxed);
        if (source.RasterIO(GF_Read,
                            x,
                            y,
                            blockWidth,
                            blockHeight,
                            destination,
                            blockWidth,
                            blockHeight,
                            type,
                            0,
                            0,
                            &extra) != CE_None) {
            checkCancelled(stop);
            throw RasterReadError("Raster elevation block read failed");
        }
    };

    for (int y = 0; y < height; y += scanBlock) {
        for (int x = 0; x < width; x += scanBlock) {
            checkCancelled(stop);
            const int blockWidth = std::min(scanBlock, width - x);
            const int blockHeight = std::min(scanBlock, height - y);
            const std::size_t count =
                static_cast<std::size_t>(blockWidth) * blockHeight;
            values.resize(count);
            read(*band,
                 x,
                 y,
                 blockWidth,
                 blockHeight,
                 GDT_Float64,
                 values.data());
            if (alpha != nullptr) {
                alphaValues.resize(count);
                read(*alpha,
                     x,
                     y,
                     blockWidth,
                     blockHeight,
                     GDT_Float64,
                     alphaValues.data());
            }
            if (mask != nullptr) {
                maskValues.resize(count);
                read(*mask,
                     x,
                     y,
                     blockWidth,
                     blockHeight,
                     GDT_Byte,
                     maskValues.data());
            }

            for (std::size_t index = 0; index < count; ++index) {
                const double raw = values[index];
                if (!std::isfinite(raw) || matchesNodata(raw, nodata) ||
                    (alpha != nullptr &&
                     (!std::isfinite(alphaValues[index]) ||
                      alphaValues[index] <= 0.0)) ||
                    (mask != nullptr && maskValues[index] == 0)) {
                    continue;
                }
                const double scaled = raw * scale + offset;
                if (!std::isfinite(scaled)) {
                    continue;
                }
                minimum = std::min(minimum, scaled);
                maximum = std::max(maximum, scaled);
            }
            ++processed;
            if (progress) {
                progress({.processedBlocks = processed, .totalBlocks = total});
            }
        }
    }
    if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
        throw RasterReadError("Raster has no valid elevation samples");
    }
    return {.minimum = minimum, .maximum = maximum};
}

std::uint64_t
GdalRasterSource::readReservationBytes(const RasterTileRequest &request) const
{
    if (request.key.levelIndex >= metadata_.levels.size()) {
        throw RasterReadError("Raster tile names an unknown level");
    }
    const RasterLevel &level = metadata_.levels[request.key.levelIndex];
    const TileWindow window = tileWindow(level, request.key);
    const auto pixels = checkedMultiply<std::uint64_t>(
        static_cast<std::uint64_t>(window.readWidth),
        static_cast<std::uint64_t>(window.readHeight));
    if (!pixels) {
        throw RasterReadError("Raster tile scratch size overflows");
    }

    std::uint64_t bytes = sizeof(RasterTileData) + rasterStoredTileBytes;
    if (request.profile == RasterTilePayloadProfile::RenderElevation) {
        bytes += static_cast<std::uint64_t>(rasterStoredTilePixels) *
                 rasterStoredTilePixels * sizeof(float);
    }
    const auto addPlane = [&](const std::uint64_t sampleBytes,
                              const std::uint64_t count = 1) {
        const auto samples = checkedMultiply(*pixels, sampleBytes);
        const auto planes =
            samples ? checkedMultiply(*samples, count) : std::nullopt;
        const auto total = planes ? checkedAdd(bytes, *planes) : std::nullopt;
        if (!total) {
            throw RasterReadError("Raster tile scratch size overflows");
        }
        bytes = *total;
    };

    addPlane(selection_.byteColorBands ? 1U : sizeof(double),
             selection_.colorBands.size());
    if (selection_.alphaBand != 0) {
        addPlane(selection_.byteAlphaBand ? 1U : sizeof(double));
    }
    if (level.maskBand) {
        addPlane(1);
    }
    if (bytes > rasterMaximumTileReadReservationBytes) {
        throw RasterReadError(
            "Raster tile decode exceeds the bounded scratch allowance");
    }
    return bytes;
}

RasterTileData GdalRasterSource::readTile(const RasterTileRequest &request,
                                          std::stop_token stop) const
{
    try {
        checkCancelled(stop);
        if (request.key.levelIndex >= metadata_.levels.size()) {
            throw RasterReadError("Raster tile names an unknown level");
        }
        const RasterLevel &level = metadata_.levels[request.key.levelIndex];
        const HandleLease lease(*handles_, acquireHandle(*handles_, stop));
        const TileWindow window = tileWindow(level, request.key);
        // Backed levels remain exact 1:1 reads. Generated coverage levels map
        // the same fixed-size output tile onto a floating window in the
        // coarsest source-backed band and let GDAL resample it in memory.
        const ReadExtent extent = readExtentFor(lease.dataset(), level, window);
        const ChannelPlanes planes = readPlanes(
            lease.dataset(), level, selection_, extent, stop, readCount_);

        checkCancelled(stop);
        const RasterDecodeParameters &decode =
            request.decode ? *request.decode : metadata_.defaultDisplay;
        const DecodeTarget target{
            .width = static_cast<int>(rasterStoredTilePixels),
            .height = static_cast<int>(rasterStoredTilePixels),
            .offsetX = window.destX,
            .offsetY = window.destY,
        };

        RasterTileData tile;
        tile.key = request.key;
        tile.renderGeneration = request.renderGeneration;
        tile.validWidth = static_cast<std::uint16_t>(window.validWidth);
        tile.validHeight = static_cast<std::uint16_t>(window.validHeight);
        tile.profile = request.profile;
        ExpandedTile expanded = expandPremultipliedRgba(
            planes,
            extent,
            target,
            selection_,
            decode,
            request.profile,
            metadata_.elevation.anchor);
        tile.rgba = std::move(expanded.rgba);
        tile.elevation = std::move(expanded.elevation);
        tile.elevationMinimum = expanded.elevationMinimum;
        tile.elevationMaximum = expanded.elevationMaximum;
        tile.hasValidElevation = expanded.hasValidElevation;
        tile.hasTranslucentAlpha = expanded.hasTranslucentAlpha;
        if (request.profile == RasterTilePayloadProfile::RenderElevation &&
            tile.elevation.size() !=
                static_cast<std::size_t>(rasterStoredTilePixels) *
                    rasterStoredTilePixels) {
            throw RasterReadError("Raster elevation payload is incomplete");
        }
        return tile;
    } catch (const RasterReadCancelled &) {
        throw;
    } catch (const RasterReadError &) {
        throw;
    } catch (const std::exception &error) {
        throw RasterReadError(error.what());
    }
}

} // namespace pci
