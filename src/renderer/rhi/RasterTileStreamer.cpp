#include "renderer/rhi/RasterTileStreamer.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace pci {

RasterTileStreamer::RasterTileStreamer(const std::uint64_t cpuByteBudget,
                                       const std::uint32_t workerCount)
    : cache_(cpuByteBudget)
{
    const std::uint32_t count = std::clamp(workerCount, 1U, 8U);
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
    cache_.setByteBudget(bytes, protectedKeys);
    metrics_.cpuBytes = cache_.residentBytes();
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
        try {
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
            completions_.push_back(std::move(completion));
            pendingKeys_.erase(request.key);
            --activeReads_;
        }
        idle_.notify_all();
        notifyWake();
    }
}

void RasterTileStreamer::reconcile(const RasterLodPlan &plan,
                                   const RasterLayer &layer)
{
    const RasterFrameLayer single{.plan = &plan, .layer = &layer};
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

        auto decode = std::make_shared<const RasterDecodeParameters>(
            layer.data->metadata().defaultDisplay);

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
    // Queued work the camera has moved away from is dropped before anything
    // new is added, so the queue tracks the current view rather than its
    // history.
    const std::size_t before = queue_.size();
    std::erase_if(queue_, [&wanted](const Request &pending) {
        return !wanted.contains(pending.key);
    });
    metrics_.cancelled += before - queue_.size();

    for (Request &request : scheduled) {
        // pendingKeys_ already contains every queued request, so the ceiling
        // is measured against it alone. Adding the queue size too would halve
        // the effective limit.
        if (pendingKeys_.size() >= rasterMaximumPendingRequests) {
            break;
        }
        if (pendingKeys_.contains(request.key) ||
            std::ranges::any_of(queue_, [&request](const Request &pending) {
                return pending.key == request.key;
            })) {
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
            continue;
        }
        ++metrics_.completed;
        if (!cache_.insert(
                completion.key, std::move(*completion.tile), protectedKeys)) {
            // The decoded cache is full of tiles that are on screen, so the
            // result is dropped rather than evicting what is being drawn.
            ++metrics_.rejectedUnadmitted;
            continue;
        }
        ++metrics_.admitted;
        pendingUploads_.push_back(completion.key);
        admitted.push_back(completion.key);
    }

    metrics_.cpuBytes = cache_.residentBytes();
    metrics_.cpuPeakBytes = std::max(metrics_.cpuPeakBytes, metrics_.cpuBytes);
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
    std::erase_if(negativeCache_, [sourceId](const auto &entry) {
        return entry.first.sourceId == sourceId;
    });
    std::erase_if(pendingUploads_, [sourceId](const RasterCacheKey &key) {
        return key.sourceId == sourceId;
    });

    const std::scoped_lock lock(mutex_);
    std::erase_if(queue_, [sourceId](const Request &pending) {
        return pending.key.sourceId == sourceId;
    });
    std::erase_if(completions_, [sourceId](const Completion &completion) {
        return completion.key.sourceId == sourceId;
    });
    metrics_.cpuBytes = cache_.residentBytes();
}

void RasterTileStreamer::waitForIdle()
{
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] {
        return queue_.empty() && activeReads_ == 0;
    });
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
    workers_.clear();
    {
        const std::scoped_lock lock(mutex_);
        completions_.clear();
        pendingKeys_.clear();
    }
    pendingUploads_.clear();
    cache_.clear();
}

RasterStreamerMetrics RasterTileStreamer::metrics() const
{
    RasterStreamerMetrics result = metrics_;
    {
        const std::scoped_lock lock(mutex_);
        result.pending = pendingKeys_.size();
        result.queued = queue_.size();
    }
    result.pendingUploads = pendingUploads_.size();
    result.negativeEntries = negativeCache_.size();
    return result;
}

} // namespace pci
