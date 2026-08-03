#include "renderer/telemetry/RenderTelemetryAccumulator.h"

#include "foundation/CheckedArithmetic.h"

namespace pci {
namespace {

std::uint64_t nanosecondsCount(const std::chrono::nanoseconds value) noexcept
{
    if (value.count() <= 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(value.count());
}

} // namespace

void RenderTelemetryAccumulator::setSourceTotals(
    const std::uint64_t sourcePoints,
    const std::uint64_t retainedFlatPoints) noexcept
{
    latest_.selection.sourcePoints = sourcePoints;
    latest_.selection.retainedFlatPoints = retainedFlatPoints;
}

void RenderTelemetryAccumulator::setPicking(
    const RenderPickingTelemetry picking) noexcept
{
    latest_.picking = picking;
}

void RenderTelemetryAccumulator::recordFrame(
    const RenderTelemetryFrameSample &sample) noexcept
{
    constexpr double smoothing = 0.1;
    if (sample.frameTime.count() > 0.0) {
        double &smoothed = latest_.frame.frameMilliseconds;
        smoothed = smoothed == 0.0 ? sample.frameTime.count()
                                   : (1.0 - smoothing) * smoothed +
                                         smoothing * sample.frameTime.count();
        latest_.frame.framesPerSecond = 1000.0 / smoothed;
    }
    latest_.frame.submittedFrameCount =
        saturatingAdd(latest_.frame.submittedFrameCount, std::uint64_t{1});
    latest_.frame.includedUploads = sample.includedUploads;
    latest_.frame.includedPick = sample.includedPick;
    latest_.selection.requestedPoints = sample.requestedPoints;
    latest_.selection.selectedPoints = sample.selectedPoints;
    latest_.selection.submittedPoints = sample.submittedPoints;
    latest_.selection.drawCalls = sample.drawCalls;
    latest_.selection.vectorLayersDrawn = sample.vectorLayersDrawn;
    latest_.selection.vectorDrawCalls = sample.vectorDrawCalls;
    latest_.selection.visibleBlocks = sample.visibleBlocks;
    latest_.selection.culledBlocks = sample.culledBlocks;
    latest_.selection.visibleLayerCount = sample.visibleLayerCount;
    latest_.selection.coveredLayerCount = sample.coveredLayerCount;
    latest_.selection.framePlanReused = sample.framePlanReused;
    latest_.upload.pendingUploadBytes = sample.pendingUploadBytes;

    samples_ = saturatingAdd(samples_, std::uint64_t{1});
    snapshotNanoseconds_ = saturatingAdd(snapshotNanoseconds_,
                                         nanosecondsCount(sample.snapshotTime));
    selectionNanoseconds_ = saturatingAdd(
        selectionNanoseconds_, nanosecondsCount(sample.selectionTime));
    uploadNanoseconds_ =
        saturatingAdd(uploadNanoseconds_, nanosecondsCount(sample.uploadTime));
    commandNanoseconds_ = saturatingAdd(commandNanoseconds_,
                                        nanosecondsCount(sample.commandTime));
    uploadedPointBytes_ =
        saturatingAdd(uploadedPointBytes_, sample.uploadedPointBytes);
    uploadOperations_ =
        saturatingAdd(uploadOperations_, sample.uploadOperations);
    resourceUpdateBatches_ =
        saturatingAdd(resourceUpdateBatches_, sample.resourceUpdateBatches);
    uniformUpdateOperations_ =
        saturatingAdd(uniformUpdateOperations_, sample.uniformUpdateOperations);
    if (sample.includedUploads || sample.includedPick) {
        specialFrameCooldown_ = 3;
    }
}

void RenderTelemetryAccumulator::advanceSpecialFrameCooldown() noexcept
{
    if (specialFrameCooldown_ > 0) {
        --specialFrameCooldown_;
    }
}

void RenderTelemetryAccumulator::makeNextSnapshotDue(
    const TimePoint now) noexcept
{
    nextPublish_ = now;
}

std::optional<RenderTelemetrySnapshot>
RenderTelemetryAccumulator::takeSnapshot(const TimePoint now,
                                         const bool force) noexcept
{
    if (!force && now < nextPublish_) {
        return std::nullopt;
    }
    nextPublish_ = now + std::chrono::milliseconds(250);
    RenderTelemetrySnapshot snapshot = latest_;
    const double divisor =
        samples_ > 0 ? static_cast<double>(samples_) * 1'000'000.0 : 1.0;
    snapshot.frame.sceneSnapshotMilliseconds =
        static_cast<double>(snapshotNanoseconds_) / divisor;
    snapshot.frame.selectionMilliseconds =
        static_cast<double>(selectionNanoseconds_) / divisor;
    snapshot.frame.uploadMilliseconds =
        static_cast<double>(uploadNanoseconds_) / divisor;
    snapshot.frame.commandRecordingMilliseconds =
        static_cast<double>(commandNanoseconds_) / divisor;
    snapshot.upload.uploadedPointBytes = uploadedPointBytes_;
    snapshot.upload.uploadOperations = uploadOperations_;
    snapshot.upload.resourceUpdateBatches = resourceUpdateBatches_;
    snapshot.upload.uniformUpdateOperations = uniformUpdateOperations_;

    samples_ = 0;
    snapshotNanoseconds_ = 0;
    selectionNanoseconds_ = 0;
    uploadNanoseconds_ = 0;
    commandNanoseconds_ = 0;
    uploadedPointBytes_ = 0;
    uploadOperations_ = 0;
    resourceUpdateBatches_ = 0;
    uniformUpdateOperations_ = 0;
    return snapshot;
}

std::uint64_t RenderTelemetryAccumulator::frameCount() const noexcept
{
    return latest_.frame.submittedFrameCount;
}

std::uint64_t RenderTelemetryAccumulator::nextFrameNumber() const noexcept
{
    return saturatingAdd(frameCount(), std::uint64_t{1});
}

std::uint64_t RenderTelemetryAccumulator::submittedPoints() const noexcept
{
    return latest_.selection.submittedPoints;
}

bool RenderTelemetryAccumulator::previousFrameIncludedUploads() const noexcept
{
    return latest_.frame.includedUploads;
}

bool RenderTelemetryAccumulator::previousFrameIncludedPick() const noexcept
{
    return latest_.frame.includedPick;
}

bool RenderTelemetryAccumulator::specialFrameCooldownActive() const noexcept
{
    return specialFrameCooldown_ > 0;
}

RenderMetrics projectRenderMetrics(const RenderTelemetrySnapshot &telemetry)
{
    return {
        .deviceName = telemetry.backend.deviceName,
        .requestedBackend = telemetry.backend.requestedBackend,
        .selectedBackend = telemetry.backend.selectedBackend,
        .timingSource = telemetry.backend.timingSource,
        .gpuValidationEnabled = telemetry.backend.gpuValidationEnabled,
        .framesPerSecond = telemetry.frame.framesPerSecond,
        .frameMilliseconds = telemetry.frame.frameMilliseconds,
        .requestedPoints = telemetry.selection.requestedPoints,
        .selectedPoints = telemetry.selection.selectedPoints,
        .submittedPoints = telemetry.selection.submittedPoints,
        .drawCalls = telemetry.selection.drawCalls,
        .vectorLayersDrawn = telemetry.selection.vectorLayersDrawn,
        .vectorDrawCalls = telemetry.selection.vectorDrawCalls,
        .gpuVectorBytes = telemetry.residency.gpuVectorBytes,
        .submittedFrameCount = telemetry.frame.submittedFrameCount,
        .sourcePoints = telemetry.selection.sourcePoints,
        .retainedFlatPoints = telemetry.selection.retainedFlatPoints,
        .decodedResidentPoints = telemetry.decode.decodedResidentPoints,
        .gpuResidentPoints = telemetry.residency.gpuResidentPoints,
        .visibleBlocks = telemetry.selection.visibleBlocks,
        .culledBlocks = telemetry.selection.culledBlocks,
        .framePlanReused = telemetry.selection.framePlanReused,
        .fullDetailWarming = telemetry.residency.fullDetailWarming,
        .fullDetailActive = telemetry.residency.fullDetailActive,
        .fullDetailDecodedNodes = telemetry.residency.fullDetailDecodedNodes,
        .fullDetailTotalNodes = telemetry.residency.fullDetailTotalNodes,
        .fullDetailDecodesInFlight =
            telemetry.residency.fullDetailDecodesInFlight,
        .uniformDrawCapacity = telemetry.upload.uniformDrawCapacity,
        .uniformCapacityGrowthCount =
            telemetry.upload.uniformCapacityGrowthCount,
        .uniformUpdateOperations = telemetry.upload.uniformUpdateOperations,
        .uploadedPointBytes = telemetry.upload.uploadedPointBytes,
        .pendingUploadBytes = telemetry.upload.pendingUploadBytes,
        .uploadOperations = telemetry.upload.uploadOperations,
        .uploadResourceUpdateBatches = telemetry.upload.resourceUpdateBatches,
        .sceneSnapshotMilliseconds = telemetry.frame.sceneSnapshotMilliseconds,
        .selectionMilliseconds = telemetry.frame.selectionMilliseconds,
        .uploadMilliseconds = telemetry.frame.uploadMilliseconds,
        .commandRecordingMilliseconds =
            telemetry.frame.commandRecordingMilliseconds,
        .frameIncludedUploads = telemetry.frame.includedUploads,
        .frameIncludedPick = telemetry.frame.includedPick,
        .pickInputBlocks = telemetry.picking.inputBlocks,
        .pickCandidateBlocks = telemetry.picking.candidateBlocks,
        .pickInputPoints = telemetry.picking.inputPoints,
        .pickCandidatePoints = telemetry.picking.candidatePoints,
        .lodRefinePixelError = telemetry.selection.lodRefinePixelError,
        .gpuPointBudgetBytes = telemetry.residency.gpuPointBudgetBytes,
        .decodedPointBytes = telemetry.decode.decodedPointBytes,
        .retainedFlatBytes = telemetry.decode.retainedFlatBytes,
        .decodedPointBudgetBytes = telemetry.decode.decodedPointBudgetBytes,
        .peakDecodedPointBytes = telemetry.decode.peakDecodedPointBytes,
        .gpuPointBytes = telemetry.residency.gpuPointBytes,
        .peakGpuPointBytes = telemetry.residency.peakGpuPointBytes,
        .gpuCacheEvictions = telemetry.residency.gpuCacheEvictions,
        .cacheHits = telemetry.decode.cacheHits,
        .cacheMisses = telemetry.decode.cacheMisses,
        .cacheEvictions = telemetry.decode.cacheEvictions,
        .sourceRequests = telemetry.decode.sourceRequests,
        .sourceRequestsCompleted = telemetry.decode.sourceRequestsCompleted,
        .sourceRequestsCancelled = telemetry.decode.sourceRequestsCancelled,
        .sourceRequestsFailed = telemetry.decode.sourceRequestsFailed,
        .estimatedSourceBytesRequested =
            telemetry.decode.estimatedSourceBytesRequested,
        .decodedSourceBytesProduced =
            telemetry.decode.decodedSourceBytesProduced,
        .decodeRequestsQueued = telemetry.decode.decodeRequestsQueued,
        .decodeRequestsStarted = telemetry.decode.decodeRequestsStarted,
        .decodeRequestsCompleted = telemetry.decode.decodeRequestsCompleted,
        .decodeRequestsCancelled = telemetry.decode.decodeRequestsCancelled,
        .decodeRequestsFailed = telemetry.decode.decodeRequestsFailed,
        .activeDecoderEstimatedBytes =
            telemetry.decode.activeDecoderEstimatedBytes,
        .peakDecoderEstimatedBytes = telemetry.decode.peakDecoderEstimatedBytes,
        .activeDecodes = telemetry.decode.activeDecodes,
        .pendingDecodes = telemetry.decode.pendingDecodes,
        .persistentIndexBytes = telemetry.decode.persistentIndexBytes,
        .localPersistentSources = telemetry.decode.localPersistentSources,
        .reusedPersistentSources = telemetry.decode.reusedPersistentSources,
        .visibleLayerCount = telemetry.selection.visibleLayerCount,
        .coveredLayerCount = telemetry.selection.coveredLayerCount,
        .processResidentBytes = telemetry.residency.processResidentBytes,
        .peakProcessResidentBytes =
            telemetry.residency.peakProcessResidentBytes,
    };
}

} // namespace pci
