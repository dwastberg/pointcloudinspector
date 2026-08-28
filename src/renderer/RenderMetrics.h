#pragma once

#include <QString>

#include <cstdint>

namespace pci {

struct RenderMetrics {
    QString deviceName;
    QString requestedBackend;
    QString selectedBackend;
    QString timingSource;
    bool gpuValidationEnabled = false;
    double framesPerSecond = 0.0;
    double frameMilliseconds = 0.0;
    // Geometry budget requested by the adaptive controller for the latest
    // frame, before visibility, residency, and upload readiness are applied.
    std::uint64_t requestedPoints = 0;
    // Points selected by frame planning and points actually backed by a GPU
    // buffer and submitted to draw calls. Their difference exposes upload or
    // residency shortfalls without conflating either value with source size.
    std::uint64_t selectedPoints = 0;
    std::uint64_t submittedPoints = 0;
    std::uint64_t drawCalls = 0;
    std::uint64_t vectorLayersDrawn = 0;
    std::uint64_t vectorDrawCalls = 0;
    std::uint64_t gpuVectorBytes = 0;
    // Raster streaming. The GDAL block cache is the third allocator holding
    // raster pixels, but it is a process-global figure the application owns:
    // the renderer must not reach GDAL, so it reports only what it allocates.
    std::uint64_t rasterCpuBytes = 0;
    std::uint64_t rasterCpuBudgetBytes = 0;
    std::uint64_t rasterCpuPeakBytes = 0;
    std::uint64_t rasterGpuBytes = 0;
    std::uint64_t rasterHeightGpuBytes = 0;
    std::uint64_t rasterGpuBudgetBytes = 0;
    std::uint64_t rasterGpuPeakBytes = 0;
    std::uint64_t rasterTilesRequested = 0;
    std::uint64_t rasterTilesCompleted = 0;
    std::uint64_t rasterTilesCancelled = 0;
    std::uint64_t rasterTilesFailed = 0;
    std::uint64_t rasterCacheEvictions = 0;
    std::uint64_t rasterUploadedTiles = 0;
    std::size_t rasterResidentTiles = 0;
    std::size_t rasterSelectedTiles = 0;
    std::size_t rasterDrawnTiles = 0;
    std::size_t rasterSurfaceDrawnTiles = 0;
    std::uint64_t rasterSurfaceTriangles = 0;
    std::size_t rasterPendingReads = 0;
    std::uint32_t rasterFinestLevel = 0;
    std::uint32_t rasterCoarsestLevel = 0;
    bool rasterCoverageIncomplete = false;
    std::uint64_t submittedFrameCount = 0;
    std::uint64_t sourcePoints = 0;
    std::uint64_t retainedFlatPoints = 0;
    std::uint64_t decodedResidentPoints = 0;
    std::uint64_t gpuResidentPoints = 0;
    std::uint64_t visibleBlocks = 0;
    std::uint64_t culledBlocks = 0;
    bool framePlanReused = false;
    std::uint64_t uniformDrawCapacity = 0;
    std::uint64_t uniformCapacityGrowthCount = 0;
    std::uint64_t uniformUpdateOperations = 0;
    std::uint64_t uploadedPointBytes = 0;
    std::uint64_t pendingUploadBytes = 0;
    std::uint64_t uploadOperations = 0;
    std::uint64_t uploadResourceUpdateBatches = 0;
    double sceneSnapshotMilliseconds = 0.0;
    double selectionMilliseconds = 0.0;
    double uploadMilliseconds = 0.0;
    double commandRecordingMilliseconds = 0.0;
    bool frameIncludedUploads = false;
    bool frameIncludedPick = false;
    std::uint64_t pickInputBlocks = 0;
    std::uint64_t pickCandidateBlocks = 0;
    std::uint64_t pickInputPoints = 0;
    std::uint64_t pickCandidatePoints = 0;
    double lodRefinePixelError = 0.0;
    std::uint64_t gpuPointBudgetBytes = 0;
    std::uint64_t decodedPointBytes = 0;
    std::uint64_t retainedFlatBytes = 0;
    std::uint64_t decodedPointBudgetBytes = 0;
    std::uint64_t peakDecodedPointBytes = 0;
    std::uint64_t gpuPointBytes = 0;
    std::uint64_t peakGpuPointBytes = 0;
    std::uint64_t gpuCacheEvictions = 0;
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
    std::uint64_t activeColorTableBytes = 0;
    std::uint64_t flatDisplacedColorBytes = 0;
    std::uint64_t retainedSourceRootBytes = 0;
    std::uint64_t retainedColoredRootBytes = 0;
    std::uint64_t visibleLayerCount = 0;
    std::uint64_t coveredLayerCount = 0;
    std::uint64_t processResidentBytes = 0;
    std::uint64_t peakProcessResidentBytes = 0;
};

} // namespace pci
