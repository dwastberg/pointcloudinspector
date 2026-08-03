#pragma once

#include "renderer/RenderMetrics.h"

#include <QString>

#include <chrono>
#include <cstdint>
#include <optional>

namespace pci {

struct RenderBackendTelemetry {
    QString deviceName;
    QString requestedBackend;
    QString selectedBackend;
    QString timingSource;
    bool gpuValidationEnabled = false;
};

struct RenderFrameTelemetry {
    double framesPerSecond = 0.0;
    double frameMilliseconds = 0.0;
    std::uint64_t submittedFrameCount = 0;
    double sceneSnapshotMilliseconds = 0.0;
    double selectionMilliseconds = 0.0;
    double uploadMilliseconds = 0.0;
    double commandRecordingMilliseconds = 0.0;
    bool includedUploads = false;
    bool includedPick = false;
};

struct RenderSelectionTelemetry {
    std::uint64_t requestedPoints = 0;
    std::uint64_t selectedPoints = 0;
    std::uint64_t submittedPoints = 0;
    std::uint64_t drawCalls = 0;
    std::uint64_t vectorLayersDrawn = 0;
    std::uint64_t vectorDrawCalls = 0;
    std::uint64_t sourcePoints = 0;
    std::uint64_t retainedFlatPoints = 0;
    std::uint64_t visibleBlocks = 0;
    std::uint64_t culledBlocks = 0;
    std::uint64_t visibleLayerCount = 0;
    std::uint64_t coveredLayerCount = 0;
    bool framePlanReused = false;
    double lodRefinePixelError = 3.0;
};

struct RenderUploadTelemetry {
    std::uint64_t uniformDrawCapacity = 0;
    std::uint64_t uniformCapacityGrowthCount = 0;
    std::uint64_t uniformUpdateOperations = 0;
    std::uint64_t uploadedPointBytes = 0;
    std::uint64_t pendingUploadBytes = 0;
    std::uint64_t uploadOperations = 0;
    std::uint64_t resourceUpdateBatches = 0;
};

struct RenderResidencyTelemetry {
    std::uint64_t gpuVectorBytes = 0;
    std::uint64_t gpuResidentPoints = 0;
    std::uint64_t gpuPointBudgetBytes = 0;
    std::uint64_t gpuPointBytes = 0;
    std::uint64_t peakGpuPointBytes = 0;
    std::uint64_t gpuCacheEvictions = 0;
    std::uint64_t processResidentBytes = 0;
    std::uint64_t peakProcessResidentBytes = 0;
    bool fullDetailWarming = false;
    bool fullDetailActive = false;
    std::uint64_t fullDetailDecodedNodes = 0;
    std::uint64_t fullDetailTotalNodes = 0;
    std::uint64_t fullDetailDecodesInFlight = 0;
};

struct RenderDecodeTelemetry {
    std::uint64_t decodedResidentPoints = 0;
    std::uint64_t decodedPointBytes = 0;
    std::uint64_t retainedFlatBytes = 0;
    std::uint64_t decodedPointBudgetBytes = 0;
    std::uint64_t peakDecodedPointBytes = 0;
    std::uint64_t cacheHits = 0;
    std::uint64_t cacheMisses = 0;
    std::uint64_t cacheEvictions = 0;
    std::uint64_t sourceRequests = 0;
    std::uint64_t sourceRequestsCompleted = 0;
    std::uint64_t sourceRequestsCancelled = 0;
    std::uint64_t sourceRequestsFailed = 0;
    std::uint64_t estimatedSourceBytesRequested = 0;
    std::uint64_t decodedSourceBytesProduced = 0;
    std::uint64_t decodeRequestsQueued = 0;
    std::uint64_t decodeRequestsStarted = 0;
    std::uint64_t decodeRequestsCompleted = 0;
    std::uint64_t decodeRequestsCancelled = 0;
    std::uint64_t decodeRequestsFailed = 0;
    std::uint64_t activeDecoderEstimatedBytes = 0;
    std::uint64_t peakDecoderEstimatedBytes = 0;
    std::uint64_t activeDecodes = 0;
    std::uint64_t pendingDecodes = 0;
    std::uint64_t persistentIndexBytes = 0;
    std::uint64_t localPersistentSources = 0;
    std::uint64_t reusedPersistentSources = 0;
};

struct RenderPickingTelemetry {
    std::uint64_t inputBlocks = 0;
    std::uint64_t candidateBlocks = 0;
    std::uint64_t inputPoints = 0;
    std::uint64_t candidatePoints = 0;
};

struct RenderTelemetrySnapshot {
    RenderBackendTelemetry backend;
    RenderFrameTelemetry frame;
    RenderSelectionTelemetry selection;
    RenderUploadTelemetry upload;
    RenderResidencyTelemetry residency;
    RenderDecodeTelemetry decode;
    RenderPickingTelemetry picking;
};

struct RenderTelemetryFrameSample {
    std::chrono::duration<double, std::milli> frameTime{};
    std::chrono::nanoseconds snapshotTime{};
    std::chrono::nanoseconds selectionTime{};
    std::chrono::nanoseconds uploadTime{};
    std::chrono::nanoseconds commandTime{};
    std::uint64_t requestedPoints = 0;
    std::uint64_t selectedPoints = 0;
    std::uint64_t submittedPoints = 0;
    std::uint64_t drawCalls = 0;
    std::uint64_t vectorLayersDrawn = 0;
    std::uint64_t vectorDrawCalls = 0;
    std::uint64_t visibleBlocks = 0;
    std::uint64_t culledBlocks = 0;
    std::uint64_t visibleLayerCount = 0;
    std::uint64_t coveredLayerCount = 0;
    bool framePlanReused = false;
    bool includedUploads = false;
    bool includedPick = false;
    std::uint64_t uploadedPointBytes = 0;
    std::uint64_t pendingUploadBytes = 0;
    std::uint64_t uploadOperations = 0;
    std::uint64_t resourceUpdateBatches = 0;
    std::uint64_t uniformUpdateOperations = 0;
};

class RenderTelemetryAccumulator {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void setSourceTotals(std::uint64_t sourcePoints,
                         std::uint64_t retainedFlatPoints) noexcept;
    void setPicking(RenderPickingTelemetry picking) noexcept;
    void recordFrame(const RenderTelemetryFrameSample &sample) noexcept;
    void advanceSpecialFrameCooldown() noexcept;
    void makeNextSnapshotDue(TimePoint now) noexcept;

    [[nodiscard]] std::optional<RenderTelemetrySnapshot>
    takeSnapshot(TimePoint now, bool force) noexcept;
    [[nodiscard]] std::uint64_t frameCount() const noexcept;
    [[nodiscard]] std::uint64_t nextFrameNumber() const noexcept;
    [[nodiscard]] std::uint64_t submittedPoints() const noexcept;
    [[nodiscard]] bool previousFrameIncludedUploads() const noexcept;
    [[nodiscard]] bool previousFrameIncludedPick() const noexcept;
    [[nodiscard]] bool specialFrameCooldownActive() const noexcept;

private:
    RenderTelemetrySnapshot latest_;
    std::uint64_t samples_ = 0;
    std::uint64_t snapshotNanoseconds_ = 0;
    std::uint64_t selectionNanoseconds_ = 0;
    std::uint64_t uploadNanoseconds_ = 0;
    std::uint64_t commandNanoseconds_ = 0;
    std::uint64_t uploadedPointBytes_ = 0;
    std::uint64_t uploadOperations_ = 0;
    std::uint64_t resourceUpdateBatches_ = 0;
    std::uint64_t uniformUpdateOperations_ = 0;
    std::uint32_t specialFrameCooldown_ = 0;
    TimePoint nextPublish_{};
};

[[nodiscard]] RenderMetrics
projectRenderMetrics(const RenderTelemetrySnapshot &telemetry);

} // namespace pci
