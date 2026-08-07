#include "import/gdal/GdalRasterSource.h"

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

struct ChannelPlanes {
    std::vector<std::vector<double>> color;
    std::vector<double> alpha;
    std::vector<double> mask;
};

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

void readWindow(GDALRasterBand &band,
                const TileWindow &window,
                std::vector<double> &destination,
                const std::stop_token &stop,
                std::atomic<std::uint64_t> &readCount)
{
    destination.assign(
        static_cast<std::size_t>(window.readWidth) * window.readHeight, 0.0);

    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.pfnProgress = abortWhenStopRequested;
    extra.pProgressData = const_cast<std::stop_token *>(&stop);

    readCount.fetch_add(1, std::memory_order_relaxed);
    // A 1:1 window: buffer dimensions equal window dimensions, so GDAL never
    // decimates and never falls back to scanning the base image.
    const CPLErr status = band.RasterIO(GF_Read,
                                        window.readX,
                                        window.readY,
                                        window.readWidth,
                                        window.readHeight,
                                        destination.data(),
                                        window.readWidth,
                                        window.readHeight,
                                        GDT_Float64,
                                        0,
                                        0,
                                        &extra);
    checkCancelled(stop);
    if (status != CE_None) {
        throw RasterReadError("Raster window read failed");
    }
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

RasterTileData expandPremultipliedRgba(const RasterTileRequest &request,
                                       const TileWindow &window,
                                       const ChannelPlanes &planes,
                                       const RasterBandSelection &selection,
                                       const RasterDecodeParameters &decode)
{
    RasterTileData tile;
    tile.key = request.key;
    tile.renderGeneration = request.renderGeneration;
    tile.validWidth = static_cast<std::uint16_t>(window.validWidth);
    tile.validHeight = static_cast<std::uint16_t>(window.validHeight);
    tile.rgba.assign(rasterStoredTileBytes, std::byte{});

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

    const auto stored = static_cast<int>(rasterStoredTilePixels);
    for (int y = 0; y < stored; ++y) {
        // Clamping the source coordinate replicates the edge texel into any
        // gutter that fell outside the level, in the same pass as the expand.
        const int sourceY =
            std::clamp(y - window.destY, 0, window.readHeight - 1);
        for (int x = 0; x < stored; ++x) {
            const int sourceX =
                std::clamp(x - window.destX, 0, window.readWidth - 1);
            const auto sourceIndex =
                static_cast<std::size_t>(sourceY) * window.readWidth + sourceX;

            bool valid = true;
            std::array<float, 4> color{0.0F, 0.0F, 0.0F, 1.0F};
            std::array<double, 3> scaled{};
            for (std::size_t channel = 0; channel < planes.color.size();
                 ++channel) {
                const double sample = planes.color[channel][sourceIndex];
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
                if (planes.color.size() >= 3) {
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        color[channel] =
                            static_cast<float>(normalize(scaled[channel]));
                    }
                } else {
                    const auto gray = static_cast<float>(normalize(first));
                    color = {gray, gray, gray, 1.0F};
                }
                break;
            case RasterSampleKind::ContinuousScalar:
                color = ramp == nullptr
                            ? std::array<float, 4>{static_cast<float>(
                                                       normalize(first)),
                                                   static_cast<float>(
                                                       normalize(first)),
                                                   static_cast<float>(
                                                       normalize(first)),
                                                   1.0F}
                            : sampleRamp(*ramp, normalize(first));
                break;
            case RasterSampleKind::Categorical: {
                const auto entry = static_cast<std::size_t>(
                    std::max<double>(0.0, std::lround(first)));
                color = entry < selection.paletteTable.size()
                            ? selection.paletteTable[entry]
                            : std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F};
                break;
            }
            }

            // colorTableAlpha * explicitAlpha * validity, composed before
            // premultiplication so the GPU's linear filter never blends an
            // invalid source color into a valid neighbour.
            double alpha = color[3];
            if (!planes.alpha.empty()) {
                alpha *=
                    std::clamp(planes.alpha[sourceIndex] / 255.0, 0.0, 1.0);
            }
            if (!planes.mask.empty()) {
                alpha *= planes.mask[sourceIndex] > 0.0 ? 1.0 : 0.0;
            }
            if (!valid) {
                alpha = 0.0;
            }

            const auto destination =
                (static_cast<std::size_t>(y) * rasterStoredTilePixels + x) * 4;
            tile.rgba[destination] = quantize(color[0] * alpha);
            tile.rgba[destination + 1] = quantize(color[1] * alpha);
            tile.rgba[destination + 2] = quantize(color[2] * alpha);
            tile.rgba[destination + 3] = quantize(alpha);
        }
    }
    return tile;
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

std::uint64_t GdalRasterSource::readCount() const noexcept
{
    return readCount_.load(std::memory_order_relaxed);
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
        const TileWindow window = tileWindow(level, request.key);

        const HandleLease lease(*handles_, acquireHandle(*handles_, stop));

        ChannelPlanes planes;
        planes.color.resize(selection_.colorBands.size());
        for (std::size_t channel = 0; channel < selection_.colorBands.size();
             ++channel) {
            checkCancelled(stop);
            readWindow(*levelBand(lease.dataset(), level.rgbaBands[channel]),
                       window,
                       planes.color[channel],
                       stop,
                       readCount_);
        }
        if (selection_.alphaBand != 0) {
            checkCancelled(stop);
            readWindow(*levelBand(lease.dataset(), level.rgbaBands[3]),
                       window,
                       planes.alpha,
                       stop,
                       readCount_);
        }
        if (level.maskBand) {
            checkCancelled(stop);
            // The mask is taken from the same overview band as the color, so
            // its dimensions match by construction rather than by a separate
            // intersection that a driver could violate.
            GDALRasterBand *mask =
                levelBand(lease.dataset(), *level.maskBand)->GetMaskBand();
            if (mask == nullptr) {
                throw RasterReadError("Raster mask band is unavailable");
            }
            readWindow(*mask, window, planes.mask, stop, readCount_);
        }

        checkCancelled(stop);
        const RasterDecodeParameters &decode =
            request.decode ? *request.decode : metadata_.defaultDisplay;
        return expandPremultipliedRgba(
            request, window, planes, selection_, decode);
    } catch (const RasterReadCancelled &) {
        throw;
    } catch (const RasterReadError &) {
        throw;
    } catch (const std::exception &error) {
        throw RasterReadError(error.what());
    }
}

} // namespace pci
