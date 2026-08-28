#include "app/RenderDiagnosticsFormatter.h"

namespace pci {

QString RenderDiagnosticsFormatter::panelText(
    const RenderMetrics &metrics,
    const PointCloudLoadControllerMetrics &loadMetrics,
    const std::optional<std::uint64_t> gdalCacheUsedBytes,
    const std::optional<std::uint64_t> gdalCacheBudgetBytes,
    const PointCloudColorizeControllerMetrics colorizeMetrics)
{
    constexpr double bytesPerMiB = 1024.0 * 1024.0;
    const auto mib = [](const std::uint64_t bytes) {
        return static_cast<double>(bytes) / bytesPerMiB;
    };
    // All three raster allocators on one line, because the point of budgeting
    // them is being able to see them together.
    const QString rasterMemory =
        QStringLiteral("Raster memory: CPU %1 / %2 MiB (peak %3), GPU %4 / "
                       "%5 MiB (peak %6), GDAL block cache %7 / %8")
            .arg(mib(metrics.rasterCpuBytes), 0, 'f', 1)
            .arg(mib(metrics.rasterCpuBudgetBytes), 0, 'f', 1)
            .arg(mib(metrics.rasterCpuPeakBytes), 0, 'f', 1)
            .arg(mib(metrics.rasterGpuBytes), 0, 'f', 1)
            .arg(mib(metrics.rasterGpuBudgetBytes), 0, 'f', 1)
            .arg(mib(metrics.rasterGpuPeakBytes), 0, 'f', 1)
            .arg(gdalCacheUsedBytes ? QStringLiteral("%1 MiB").arg(
                                          mib(*gdalCacheUsedBytes), 0, 'f', 1)
                                    : QStringLiteral("unavailable"))
            .arg(gdalCacheBudgetBytes
                     ? QStringLiteral("%1 MiB").arg(
                           mib(*gdalCacheBudgetBytes), 0, 'f', 1)
                     : QStringLiteral("unavailable"));
    const QString rasterTiles =
        QStringLiteral("Raster tiles: %1 drawn / %2 selected / %3 resident, "
                       "%4 pending, levels %5-%6, coverage %7; Surface %8 "
                       "draws / %9 triangles, height GPU %10 MiB")
            .arg(metrics.rasterDrawnTiles)
            .arg(metrics.rasterSelectedTiles)
            .arg(metrics.rasterResidentTiles)
            .arg(metrics.rasterPendingReads)
            .arg(metrics.rasterFinestLevel)
            .arg(metrics.rasterCoarsestLevel)
            .arg(metrics.rasterCoverageIncomplete ? QStringLiteral("partial")
                                                  : QStringLiteral("complete"))
            .arg(metrics.rasterSurfaceDrawnTiles)
            .arg(metrics.rasterSurfaceTriangles)
            .arg(mib(metrics.rasterHeightGpuBytes), 0, 'f', 1);
    const QString rasterReads =
        QStringLiteral("Raster reads: %1 requested, %2 completed, %3 "
                       "cancelled, %4 failed, %5 uploaded, %6 evictions")
            .arg(metrics.rasterTilesRequested)
            .arg(metrics.rasterTilesCompleted)
            .arg(metrics.rasterTilesCancelled)
            .arg(metrics.rasterTilesFailed)
            .arg(metrics.rasterUploadedTiles)
            .arg(metrics.rasterCacheEvictions);
    const QString rasterPointColors =
        QStringLiteral("Raster point colors: tables %1 MiB, displaced flat "
                       "%2 MiB, roots source %3 / colored %4 MiB")
            .arg(mib(metrics.activeColorTableBytes), 0, 'f', 1)
            .arg(mib(metrics.flatDisplacedColorBytes), 0, 'f', 1)
            .arg(mib(metrics.retainedSourceRootBytes), 0, 'f', 1)
            .arg(mib(metrics.retainedColoredRootBytes), 0, 'f', 1);
    const QString colorizeWork =
        QStringLiteral("Colorize work: table %1 MiB, staging %2 MiB, run "
                       "buffer %3 MiB, temporary %4 MiB; records %5 / %6, "
                       "tiles %7 attempted / %8 failed")
            .arg(mib(colorizeMetrics.activeColorTableBytes), 0, 'f', 1)
            .arg(mib(colorizeMetrics.activeStagingBytes), 0, 'f', 1)
            .arg(mib(colorizeMetrics.activeRunBufferBytes), 0, 'f', 1)
            .arg(mib(colorizeMetrics.temporaryBytesWritten), 0, 'f', 1)
            .arg(colorizeMetrics.recordsSampled)
            .arg(colorizeMetrics.recordsGenerated)
            .arg(colorizeMetrics.rasterTilesAttempted)
            .arg(colorizeMetrics.rasterTilesFailed);

    return QStringLiteral(
               "Coverage: %1 / %2 visible sources\n"
               "CPU pages: %3 / %4 MiB (peak %5)\n"
               "GPU points: %6 / %7 MiB (peak %8)\n"
               "Local indexes: %9 MiB, %10 sources (%11 reused)\n"
               "Page cache: %12 hits, %13 misses, %14 evictions\n"
               "Page decode: %15 active, %16 queued, %17 MiB active\n"
               "Import/index: %18 active, %19 queued, %20 MiB active\n"
               "Process RSS: %21 MiB (peak %22)\n"
               "%23\n%24\n%25\n%26\n%27")
        .arg(metrics.coveredLayerCount)
        .arg(metrics.visibleLayerCount)
        .arg(mib(metrics.decodedPointBytes), 0, 'f', 1)
        .arg(mib(metrics.decodedPointBudgetBytes), 0, 'f', 0)
        .arg(mib(metrics.peakDecodedPointBytes), 0, 'f', 1)
        .arg(mib(metrics.gpuPointBytes), 0, 'f', 1)
        .arg(mib(metrics.gpuPointBudgetBytes), 0, 'f', 0)
        .arg(mib(metrics.peakGpuPointBytes), 0, 'f', 1)
        .arg(mib(metrics.persistentIndexBytes), 0, 'f', 1)
        .arg(metrics.localPersistentSources)
        .arg(metrics.reusedPersistentSources)
        .arg(metrics.cacheHits)
        .arg(metrics.cacheMisses)
        .arg(metrics.cacheEvictions)
        .arg(metrics.activeDecodes)
        .arg(metrics.pendingDecodes)
        .arg(mib(metrics.activeDecoderEstimatedBytes), 0, 'f', 1)
        .arg(loadMetrics.scheduler.active)
        .arg(loadMetrics.scheduler.pending)
        .arg(mib(loadMetrics.scheduler.activeEstimatedBytes), 0, 'f', 1)
        .arg(mib(metrics.processResidentBytes), 0, 'f', 1)
        .arg(mib(metrics.peakProcessResidentBytes), 0, 'f', 1)
        .arg(rasterMemory,
             rasterTiles,
             rasterReads,
             rasterPointColors,
             colorizeWork);
}

QString RenderDiagnosticsFormatter::statusText(
    const RenderMetrics &metrics,
    const std::optional<double> timeToFirstPointsMilliseconds)
{
    constexpr double bytesPerMiB = 1024.0 * 1024.0;
    const QString rendererLabel =
        QStringLiteral("%1 [%2→%3%4]")
            .arg(metrics.deviceName,
                 metrics.requestedBackend,
                 metrics.selectedBackend,
                 metrics.gpuValidationEnabled ? QStringLiteral("+validation")
                                              : QString{});
    return QStringLiteral(
               "%1 | %2 %3ms %4FPS | P %5R/%6S/%7D | Res "
               "%8Src/%9Flat/%10CPU/%11GPU | B %12V/%13C D%14 F%15 | "
               "UBO %16/%17G/%18U | Up %19/%20 MiB %21O/%22B | Plan "
               "%23/%24/%25/%26 ms %27 | First %28 ms | Mem %29/%30 "
               "CPU %31 GPU %32/%33 RSS | Cache %34H/%35M | Decode "
               "%36A/%37Q/%38C | Pick %39/%40B %41/%42P | "
               "Raster %43D/%44S %45/%46 MiB %47Q")
        .arg(rendererLabel)
        .arg(metrics.timingSource)
        .arg(metrics.frameMilliseconds, 0, 'f', 2)
        .arg(metrics.framesPerSecond, 0, 'f', 1)
        .arg(metrics.requestedPoints)
        .arg(metrics.selectedPoints)
        .arg(metrics.submittedPoints)
        .arg(metrics.sourcePoints)
        .arg(metrics.retainedFlatPoints)
        .arg(metrics.decodedResidentPoints)
        .arg(metrics.gpuResidentPoints)
        .arg(metrics.visibleBlocks)
        .arg(metrics.culledBlocks)
        .arg(metrics.drawCalls)
        .arg(metrics.submittedFrameCount)
        .arg(metrics.uniformDrawCapacity)
        .arg(metrics.uniformCapacityGrowthCount)
        .arg(metrics.uniformUpdateOperations)
        .arg(static_cast<double>(metrics.uploadedPointBytes) / bytesPerMiB,
             0,
             'f',
             1)
        .arg(static_cast<double>(metrics.pendingUploadBytes) / bytesPerMiB,
             0,
             'f',
             1)
        .arg(metrics.uploadOperations)
        .arg(metrics.uploadResourceUpdateBatches)
        .arg(metrics.sceneSnapshotMilliseconds, 0, 'f', 2)
        .arg(metrics.selectionMilliseconds, 0, 'f', 2)
        .arg(metrics.uploadMilliseconds, 0, 'f', 2)
        .arg(metrics.commandRecordingMilliseconds, 0, 'f', 2)
        .arg(metrics.framePlanReused ? QStringLiteral("reused")
                                     : QStringLiteral("new"))
        .arg(timeToFirstPointsMilliseconds
                 ? QString::number(*timeToFirstPointsMilliseconds, 'f', 1)
                 : QStringLiteral("-"))
        .arg(static_cast<double>(metrics.decodedPointBytes) / bytesPerMiB,
             0,
             'f',
             1)
        .arg(metrics.decodedPointBudgetBytes > 0
                 ? QString::number(
                       static_cast<double>(metrics.decodedPointBudgetBytes) /
                           bytesPerMiB,
                       'f',
                       0)
                 : QStringLiteral("-"))
        .arg(
            static_cast<double>(metrics.gpuPointBytes) / bytesPerMiB, 0, 'f', 1)
        .arg(static_cast<double>(metrics.processResidentBytes) / bytesPerMiB,
             0,
             'f',
             1)
        .arg(static_cast<double>(metrics.peakProcessResidentBytes) /
                 bytesPerMiB,
             0,
             'f',
             1)
        .arg(metrics.cacheHits)
        .arg(metrics.cacheMisses)
        .arg(metrics.activeDecodes)
        .arg(metrics.pendingDecodes)
        .arg(metrics.decodeRequestsCancelled)
        .arg(metrics.pickCandidateBlocks)
        .arg(metrics.pickInputBlocks)
        .arg(metrics.pickCandidatePoints)
        .arg(metrics.pickInputPoints)
        .arg(metrics.rasterDrawnTiles)
        .arg(metrics.rasterSelectedTiles)
        .arg(static_cast<double>(metrics.rasterCpuBytes) / bytesPerMiB,
             0,
             'f',
             1)
        .arg(static_cast<double>(metrics.rasterGpuBytes) / bytesPerMiB,
             0,
             'f',
             1)
        .arg(metrics.rasterPendingReads);
}

} // namespace pci
