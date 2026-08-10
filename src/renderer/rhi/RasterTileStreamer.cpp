#include "renderer/rhi/RasterTileStreamer.h"

#include "foundation/CheckedArithmetic.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace pci {

std::shared_ptr<const RasterDecodeParameters>
resolveRasterDecodeParameters(const RasterLayer &layer,
                              const PointColorMapCatalogSnapshot &colorMaps)
{
    auto decode = std::make_shared<RasterDecodeParameters>();
    if (!layer.data) {
        return decode;
    }
    *decode = layer.data->metadata().defaultDisplay;
    if (layer.style.displayRange) {
        decode->displayRange = layer.style.displayRange;
    }
    if (decode->sampleKind != RasterSampleKind::ContinuousScalar ||
        layer.style.colorRampKey.empty()) {
        return decode;
    }

    const auto definition = std::ranges::find(
        colorMaps.definitions(),
        std::string_view(layer.style.colorRampKey),
        &PointColorMapDefinition::key);
    if (definition != colorMaps.definitions().end() &&
        definition->kind == PointColorMapKind::Continuous &&
        !definition->stops.empty()) {
        decode->colorRamp = std::make_shared<const std::vector<PointColorStop>>(
            definition->stops.begin(), definition->stops.end());
    }
    return decode;
}

RasterTileStreamer::RasterTileStreamer(const std::uint64_t cpuByteBudget,
                                       const std::uint32_t workerCount)
    : cache_(0)
{
    const std::uint32_t count = std::clamp(workerCount, 1U, 8U);
    workerCount_ = count;
    totalCpuByteBudget_ = cpuByteBudget;
    readPoolByteCapacity_ = readPoolCapacity(cpuByteBudget);
    cache_.setByteBudget(cpuByteBudget - readPoolByteCapacity_, {});
    workers_.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        workers_.emplace_back([this](std::stop_token stop) {
            workerLoop(std::move(stop));
        });
    }
}

RasterTileStreamer::~RasterTileStreamer()
{
    shutdown();
}

void RasterTileStreamer::setWakeCallback(WakeCallback wake)
{
    const std::scoped_lock lock(mutex_);
    wake_ = std::move(wake);
}

void RasterTileStreamer::setCpuByteBudget(
    const std::uint64_t bytes,
    const std::span<const RasterCacheKey> protectedKeys)
{
    std::uint64_t cacheBudget = 0;
    std::uint64_t reserved = 0;
    {
        const std::scoped_lock lock(mutex_);
        totalCpuByteBudget_ = bytes;
        readPoolByteCapacity_ = readPoolCapacity(bytes);
        reserved = reservedReadBytes_;
        // A live decrease cannot reclaim an allocation inside RasterIO. Make
        // the cache yield enough room for those already-active reservations,
        // and admit no new reads until the pool is back below its new limit.
        const std::uint64_t unavailable =
            std::max(readPoolByteCapacity_, reservedReadBytes_);
        cacheBudget = bytes > unavailable ? bytes - unavailable : 0;
    }
    cache_.setByteBudget(cacheBudget, protectedKeys);
    cacheResidentBytes_.store(cache_.residentBytes(),
                              std::memory_order_relaxed);
    updateCpuPeak(reserved);
    memoryReady_.notify_all();
}

std::uint64_t RasterTileStreamer::readPoolCapacity(
    const std::uint64_t totalBudget) const noexcept
{
    const std::uint64_t workerCeiling =
        static_cast<std::uint64_t>(workerCount_) *
        rasterMaximumTileReadReservationBytes;
    return std::min(totalBudget / 2, workerCeiling);
}

void RasterTileStreamer::updateCpuPeak(
    const std::uint64_t reservedBytes) noexcept
{
    const std::uint64_t current = saturatingAdd(
        cacheResidentBytes_.load(std::memory_order_relaxed), reservedBytes);
    std::uint64_t peak = cpuPeakBytes_.load(std::memory_order_relaxed);
    while (peak < current &&
           !cpuPeakBytes_.compare_exchange_weak(peak,
                                                current,
                                                std::memory_order_relaxed)) {
    }
}

void RasterTileStreamer::releaseCompletionReservation(
    const std::uint64_t bytes)
{
    if (bytes == 0) {
        return;
    }
    {
        const std::scoped_lock lock(mutex_);
        reservedReadBytes_ -= std::min(reservedReadBytes_, bytes);
    }
    memoryReady_.notify_all();
    idle_.notify_all();
}

void RasterTileStreamer::notifyWake()
{
    // One queued event per completed tile would flood the loop during a large
    // refinement, so wakeups collapse until the render thread drains.
    if (wakeQueued_.exchange(true)) {
        return;
    }
    WakeCallback wake;
    {
        const std::scoped_lock lock(mutex_);
        wake = wake_;
    }
    if (wake) {
        wake();
    }
}

void RasterTileStreamer::workerLoop(std::stop_token stop)
{
    for (;;) {
        Request request;
        {
            std::unique_lock lock(mutex_);
            if (!queueReady_.wait(lock, stop, [this] {
                    return !queue_.empty();
                })) {
                return;
            }
            request = std::move(queue_.front());
            queue_.pop_front();
            ++activeReads_;
        }

        Completion completion;
        completion.key = request.key;
        completion.epoch = request.epoch;
        std::uint64_t reservation = 0;
        try {
            const std::uint64_t required =
                request.source->readReservationBytes(request.request);
            {
                std::unique_lock lock(mutex_);
                ++memoryWaiters_;
                idle_.notify_all();
                const bool ready = memoryReady_.wait(lock, stop, [&, this] {
                    return required > readPoolByteCapacity_ ||
                           reservedReadBytes_ <=
                               readPoolByteCapacity_ - required;
                });
                --memoryWaiters_;
                if (!ready) {
                    throw RasterReadCancelled{};
                }
                if (required > readPoolByteCapacity_) {
                    throw RasterReadError(
                        "Raster CPU budget cannot reserve one bounded tile "
                        "read");
                }
                reservedReadBytes_ += required;
                reservation = required;
                updateCpuPeak(reservedReadBytes_);
            }
            completion.tile = request.source->readTile(request.request, stop);
        } catch (const RasterReadCancelled &) {
            // Cancellation is not a failure. It must never enter the negative
            // cache, or a cancelled pan would poison the tiles the next frame
            // needs.
            completion.cancelled = true;
        } catch (const std::exception &error) {
            completion.error = error.what();
        }

        {
            const std::scoped_lock lock(mutex_);
            const auto live = liveGenerations_.find(request.key.sourceId);
            if (live == liveGenerations_.end() ||
                live->second != request.key.renderGeneration) {
                completion.tile.reset();
                completion.error.clear();
                completion.cancelled = true;
            }
            if (completion.tile) {
                const std::uint64_t actual = completion.tile->byteSize();
                if (actual <= reservation) {
                    reservedReadBytes_ -= reservation - actual;
                    reservation = actual;
                } else {
                    const std::uint64_t extra = actual - reservation;
                    if (reservedReadBytes_ > readPoolByteCapacity_ ||
                        extra >
                            readPoolByteCapacity_ - reservedReadBytes_) {
                        completion.tile.reset();
                        completion.error =
                            "Raster source exceeded its read reservation";
                    } else {
                        reservedReadBytes_ += extra;
                        reservation = actual;
                    }
                }
            }
            if (!completion.tile) {
                reservedReadBytes_ -=
                    std::min(reservedReadBytes_, reservation);
                reservation = 0;
            }
            completion.reservedBytes = reservation;
            completions_.push_back(std::move(completion));
            pendingKeys_.erase(request.key);
            --activeReads_;
        }
        memoryReady_.notify_all();
        idle_.notify_all();
        notifyWake();
    }
}

void RasterTileStreamer::reconcile(const RasterLodPlan &plan,
                                   const RasterLayer &layer)
{
    const RasterFrameLayer single{
        .plan = &plan, .layer = &layer, .decode = {}};
    reconcile(std::span{&single, 1});
}

void RasterTileStreamer::reconcile(
    const std::span<const RasterFrameLayer> frame)
{
    ++requestEpoch_;

    std::unordered_set<RasterCacheKey> wanted;
    std::vector<Request> scheduled;

    for (const RasterFrameLayer &entry : frame) {
        if (!entry.plan || !entry.layer || !entry.layer->data) {
            continue;
        }
        const RasterLayer &layer = *entry.layer;
        const RasterLodPlan &plan = *entry.plan;
        wanted.reserve(wanted.size() + plan.requests.size());
        scheduled.reserve(scheduled.size() + plan.requests.size());

        std::shared_ptr<const RasterDecodeParameters> decode = entry.decode;
        if (!decode) {
            decode = std::make_shared<const RasterDecodeParameters>(
                layer.data->metadata().defaultDisplay);
        }

        for (const RasterTileKey key : plan.requests) {
            const RasterCacheKey cacheKey{
                .sourceId = layer.data->sourceId,
                .renderGeneration = layer.renderGeneration,
                .tile = key,
            };
            wanted.insert(cacheKey);
            // A tile already decoded, or already known to fail for this
            // generation, is not requested again.
            if (cache_.contains(cacheKey) ||
                negativeCache_.contains(cacheKey)) {
                continue;
            }
            scheduled.push_back(Request{
                .key = cacheKey,
                .epoch = requestEpoch_,
                .source = layer.data->source,
                .request =
                    RasterTileRequest{
                        .key = key,
                        .renderGeneration = layer.renderGeneration,
                        .decode = decode,
                    },
            });
        }
    }

    const std::scoped_lock lock(mutex_);
    for (const RasterFrameLayer &entry : frame) {
        if (entry.layer && entry.layer->data) {
            liveGenerations_.insert_or_assign(entry.layer->data->sourceId,
                                              entry.layer->renderGeneration);
        }
    }
    // Queued work the camera has moved away from is dropped before anything
    // new is added, so the queue tracks the current view rather than its
    // history.
    for (auto pending = queue_.begin(); pending != queue_.end();) {
        if (wanted.contains(pending->key)) {
            ++pending;
            continue;
        }
        pendingKeys_.erase(pending->key);
        pending = queue_.erase(pending);
        ++metrics_.cancelled;
    }

    for (Request &request : scheduled) {
        // pendingKeys_ already contains every queued request, so the ceiling
        // is measured against it alone. Adding the queue size too would halve
        // the effective limit.
        if (pendingKeys_.size() >= rasterMaximumPendingRequests) {
            break;
        }
        // pendingKeys_ is the whole dedupe answer: a key enters it at enqueue
        // and leaves at completion, so anything in the queue is already in it.
        // Scanning the queue as well was a linear check per scheduled request,
        // which at catalog scale is a few hundred thousand comparisons a frame
        // for an answer the hash set already holds.
        if (pendingKeys_.contains(request.key)) {
            continue;
        }
        pendingKeys_.insert(request.key);
        queue_.push_back(std::move(request));
        ++metrics_.requested;
    }
    queueReady_.notify_all();
}

std::vector<RasterCacheKey> RasterTileStreamer::drainCompletions(
    const std::span<const RasterCacheKey> protectedKeys,
    const std::size_t maximumResults)
{
    wakeQueued_.store(false);

    std::vector<Completion> drained;
    {
        const std::scoped_lock lock(mutex_);
        const std::size_t count = std::min(maximumResults, completions_.size());
        drained.reserve(count);
        std::move(completions_.begin(),
                  completions_.begin() + static_cast<std::ptrdiff_t>(count),
                  std::back_inserter(drained));
        completions_.erase(completions_.begin(),
                           completions_.begin() +
                               static_cast<std::ptrdiff_t>(count));
    }

    std::vector<RasterCacheKey> admitted;
    for (Completion &completion : drained) {
        if (completion.cancelled) {
            ++metrics_.cancelled;
            releaseCompletionReservation(completion.reservedBytes);
            continue;
        }
        // A completed read is admitted whatever epoch it was issued in. The
        // epoch advances every frame and reads outlive a frame, so rejecting
        // by epoch here would discard nearly every tile the streamer paid for.
        // Correctness comes from the cache key instead: it carries the source
        // id and the render generation, so a tile decoded for an obsolete
        // range or ramp can never be served as current. The epoch's job is to
        // drop *queued* work the camera has moved away from, which reconcile()
        // does before the read ever starts.
        if (!completion.tile) {
            // A read error is a property of the source and tile, not of the
            // camera, so it is remembered regardless of epoch.
            negativeCache_.emplace(completion.key, std::move(completion.error));
            ++metrics_.failed;
            releaseCompletionReservation(completion.reservedBytes);
            continue;
        }
        ++metrics_.completed;
        if (!cache_.insert(
                completion.key, std::move(*completion.tile), protectedKeys)) {
            // The decoded cache is full of tiles that are on screen, so the
            // result is dropped rather than evicting what is being drawn.
            ++metrics_.rejectedUnadmitted;
            releaseCompletionReservation(completion.reservedBytes);
            continue;
        }
        releaseCompletionReservation(completion.reservedBytes);
        ++metrics_.admitted;
        pendingUploads_.push_back(completion.key);
        admitted.push_back(completion.key);
    }

    cacheResidentBytes_.store(cache_.residentBytes(),
                              std::memory_order_relaxed);
    std::uint64_t reserved = 0;
    std::uint64_t desiredCacheBudget = 0;
    {
        const std::scoped_lock lock(mutex_);
        reserved = reservedReadBytes_;
        const std::uint64_t unavailable =
            std::max(readPoolByteCapacity_, reservedReadBytes_);
        desiredCacheBudget = totalCpuByteBudget_ > unavailable
                                 ? totalCpuByteBudget_ - unavailable
                                 : 0;
    }
    // Once pre-change reads finish, restore the normal cache/read-pool split.
    // Expanding is harmless; shrinking here preserves the combined invariant
    // after a live settings decrease.
    if (cache_.byteBudget() != desiredCacheBudget) {
        cache_.setByteBudget(desiredCacheBudget, protectedKeys);
        cacheResidentBytes_.store(cache_.residentBytes(),
                                  std::memory_order_relaxed);
    }
    updateCpuPeak(reserved);
    metrics_.cacheEvictions = cache_.evictions();
    return admitted;
}

std::vector<RasterCacheKey>
RasterTileStreamer::takeReadyUploads(const std::size_t maximumTiles)
{
    const std::size_t count = std::min(maximumTiles, pendingUploads_.size());
    std::vector<RasterCacheKey> ready(pendingUploads_.begin(),
                                      pendingUploads_.begin() +
                                          static_cast<std::ptrdiff_t>(count));
    pendingUploads_.erase(pendingUploads_.begin(),
                          pendingUploads_.begin() +
                              static_cast<std::ptrdiff_t>(count));
    return ready;
}

void RasterTileStreamer::requeueReadyUploads(
    const std::span<const RasterCacheKey> keys)
{
    for (const RasterCacheKey &key : keys) {
        if (pendingUploads_.size() >= rasterMaximumPendingRequests) {
            break;
        }
        if (!cache_.contains(key) ||
            std::ranges::find(pendingUploads_, key) != pendingUploads_.end()) {
            continue;
        }
        pendingUploads_.push_back(key);
    }
}

bool RasterTileStreamer::cpuResident(const RasterCacheKey &key) const noexcept
{
    return cache_.contains(key);
}

const RasterTileData *RasterTileStreamer::tile(const RasterCacheKey &key)
{
    return cache_.find(key);
}

bool RasterTileStreamer::failed(const RasterCacheKey &key) const noexcept
{
    return negativeCache_.contains(key);
}

std::optional<std::string>
RasterTileStreamer::lastError(const RasterSourceId sourceId) const
{
    for (const auto &[key, message] : negativeCache_) {
        if (key.sourceId == sourceId) {
            return message;
        }
    }
    return std::nullopt;
}

void RasterTileStreamer::releaseSource(const RasterSourceId sourceId)
{
    static_cast<void>(cache_.removeSource(sourceId));
    cacheResidentBytes_.store(cache_.residentBytes(),
                              std::memory_order_relaxed);
    std::erase_if(negativeCache_, [sourceId](const auto &entry) {
        return entry.first.sourceId == sourceId;
    });
    std::erase_if(pendingUploads_, [sourceId](const RasterCacheKey &key) {
        return key.sourceId == sourceId;
    });

    const std::scoped_lock lock(mutex_);
    liveGenerations_.erase(sourceId);
    for (auto pending = queue_.begin(); pending != queue_.end();) {
        if (pending->key.sourceId != sourceId) {
            ++pending;
            continue;
        }
        pendingKeys_.erase(pending->key);
        pending = queue_.erase(pending);
        ++metrics_.cancelled;
    }
    for (auto completion = completions_.begin();
         completion != completions_.end();) {
        if (completion->key.sourceId != sourceId) {
            ++completion;
            continue;
        }
        reservedReadBytes_ -=
            std::min(reservedReadBytes_, completion->reservedBytes);
        completion = completions_.erase(completion);
    }
    memoryReady_.notify_all();
    idle_.notify_all();
}

void RasterTileStreamer::waitForIdle(
    const std::span<const RasterCacheKey> protectedKeys)
{
    for (;;) {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] {
            return (queue_.empty() && activeReads_ == 0) ||
                   (memoryWaiters_ > 0 && !completions_.empty());
        });
        if (queue_.empty() && activeReads_ == 0) {
            return;
        }
        // Test and qualification callers sometimes wait for hundreds of
        // reads before explicitly draining. Backpressure intentionally stops
        // workers when completions fill the reservation pool; transfer a
        // bounded batch here so waitForIdle itself cannot deadlock.
        lock.unlock();
        static_cast<void>(drainCompletions(protectedKeys, 64));
    }
}

void RasterTileStreamer::shutdown() noexcept
{
    stop_.request_stop();
    {
        const std::scoped_lock lock(mutex_);
        queue_.clear();
        wake_ = nullptr;
    }
    for (std::jthread &worker : workers_) {
        worker.request_stop();
    }
    queueReady_.notify_all();
    memoryReady_.notify_all();
    workers_.clear();
    {
        const std::scoped_lock lock(mutex_);
        completions_.clear();
        pendingKeys_.clear();
        liveGenerations_.clear();
        reservedReadBytes_ = 0;
        memoryWaiters_ = 0;
    }
    pendingUploads_.clear();
    cache_.clear();
    cacheResidentBytes_.store(0, std::memory_order_relaxed);
}

RasterStreamerMetrics RasterTileStreamer::metrics() const
{
    RasterStreamerMetrics result = metrics_;
    {
        const std::scoped_lock lock(mutex_);
        result.pending = pendingKeys_.size();
        result.queued = queue_.size();
        result.cpuBytes = saturatingAdd(
            cacheResidentBytes_.load(std::memory_order_relaxed),
            reservedReadBytes_);
    }
    result.cpuPeakBytes = cpuPeakBytes_.load(std::memory_order_relaxed);
    result.pendingUploads = pendingUploads_.size();
    result.negativeEntries = negativeCache_.size();
    return result;
}

} // namespace pci
