#include <pci/desktop/ui/QualificationReporter.h>

#include <pci/adapters/platform/QtPath.h>
#include <pci/desktop/session/SceneSession.h>
#include <pci/desktop/ui/QualificationCriteria.h>
#include <pci/foundation/CheckedArithmetic.h>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSysInfo>

#include <algorithm>
#include <limits>
#include <ranges>
#include <system_error>
#include <utility>

namespace pci {

void QualificationReporter::configure(std::filesystem::path outputPath,
                                      const bool exitAfterWrite)
{
    outputPath_ = std::move(outputPath);
    exitAfterWrite_ = exitAfterWrite;
    reportWritten_ = false;
    lastRenderMetrics_.reset();
    frameMetrics_.clear();
}

bool QualificationReporter::configured() const noexcept
{
    return !outputPath_.empty();
}

void QualificationReporter::setGdalRuntimeInfo(GdalRuntimeInfo info)
{
    gdalRuntimeInfo_ = std::move(info);
}

void QualificationReporter::beginReplay()
{
    lastRenderMetrics_.reset();
    frameMetrics_.clear();
}

void QualificationReporter::record(const RenderMetrics &metrics)
{
    lastRenderMetrics_ = metrics;
    const double frameMilliseconds = metrics.sampledFrameMilliseconds > 0.0
                                         ? metrics.sampledFrameMilliseconds
                                         : metrics.frameMilliseconds;
    if (configured() && frameMilliseconds > 0.0 &&
        frameMetrics_.size() < 16'384) {
        frameMetrics_.push_back(metrics);
    }
}

QualificationWriteResult
QualificationReporter::write(const QString &status,
                             SceneSession &session,
                             const QualificationGdalCacheSnapshot gdalCache)
{
    if (!configured() || reportWritten_) {
        return {};
    }
    reportWritten_ = true;

    std::vector<double> frameTimes;
    frameTimes.reserve(frameMetrics_.size());
    for (const RenderMetrics &metrics : frameMetrics_) {
        frameTimes.push_back(metrics.sampledFrameMilliseconds > 0.0
                                 ? metrics.sampledFrameMilliseconds
                                 : metrics.frameMilliseconds);
    }
    std::ranges::sort(frameTimes);
    const auto percentile = [&frameTimes](const double fraction) {
        if (frameTimes.empty()) {
            return 0.0;
        }
        const std::size_t index = static_cast<std::size_t>(
            std::clamp(fraction * static_cast<double>(frameTimes.size() - 1U),
                       0.0,
                       static_cast<double>(frameTimes.size() - 1U)));
        return frameTimes[index];
    };

    QJsonArray sources;
    for (const PointCloudLoadJobState &state : session.pointLoadJobStates()) {
        sources.append(QJsonObject{
            {QStringLiteral("job_id"),
             static_cast<qint64>(state.jobId.value())},
            {QStringLiteral("path"), pathToQString(state.sourcePath)},
            {QStringLiteral("local_paging"), state.localPaging},
            {QStringLiteral("preview_available"), state.previewAvailable},
            {QStringLiteral("safety_sampled"), state.safetySampled},
            {QStringLiteral("error"), QString::fromStdString(state.detail)},
        });
    }

    QJsonArray layers;
    std::uint64_t persistentBytes = 0;
    std::uint64_t sourceFileBytes = 0;
    const SceneDocumentSnapshotPtr document = session.documentSnapshot();
    for (const PointCloudLayerSnapshot &layer : document->pointLayers()) {
        const PointCloudStorageMetrics storage =
            session.pointStorageMetrics(layer.id).value_or(
                PointCloudStorageMetrics{});
        persistentBytes =
            saturatingAdd(persistentBytes, storage.persistentBytes);
        std::error_code fileSizeError;
        const std::uintmax_t layerFileBytes = std::filesystem::file_size(
            layer.descriptor.metadata.sourcePath, fileSizeError);
        const std::uint64_t boundedLayerFileBytes =
            fileSizeError ? 0
            : layerFileBytes > std::numeric_limits<std::uint64_t>::max()
                ? std::numeric_limits<std::uint64_t>::max()
                : static_cast<std::uint64_t>(layerFileBytes);
        sourceFileBytes = saturatingAdd(sourceFileBytes, boundedLayerFileBytes);
        layers.append(QJsonObject{
            {QStringLiteral("layer_id"), static_cast<qint64>(layer.id.value())},
            {QStringLiteral("path"),
             pathToQString(layer.descriptor.metadata.sourcePath)},
            {QStringLiteral("source_points"),
             static_cast<qint64>(layer.descriptor.metadata.sourcePointCount)},
            {QStringLiteral("source_file_bytes"),
             static_cast<qint64>(boundedLayerFileBytes)},
            {QStringLiteral("index_bytes"),
             static_cast<qint64>(storage.persistentBytes)},
            {QStringLiteral("index_reused"), storage.reused},
            {QStringLiteral("visible"), layer.visible},
        });
    }

    QJsonArray rasterLayers;
    for (const RasterLayerSnapshot &layer : document->rasterLayers()) {
        const RasterLayerMetadata &raster = layer.descriptor.metadata;
        rasterLayers.append(QJsonObject{
            {QStringLiteral("layer_id"), static_cast<qint64>(layer.id.value())},
            {QStringLiteral("path"), pathToQString(raster.sourcePath)},
            {QStringLiteral("driver"),
             QString::fromStdString(raster.sourceDriver)},
            {QStringLiteral("width"), static_cast<qint64>(raster.width)},
            {QStringLiteral("height"), static_cast<qint64>(raster.height)},
            {QStringLiteral("bands"), static_cast<qint64>(raster.bands.size())},
            {QStringLiteral("levels"),
             static_cast<qint64>(raster.levels.size())},
            {QStringLiteral("backed_levels"),
             static_cast<qint64>(rasterBackedLevelCount(raster.levels))},
            {QStringLiteral("generated_levels"),
             static_cast<qint64>(rasterGeneratedLevelCount(raster.levels))},
            {QStringLiteral("coarsest_level_width"),
             static_cast<qint64>(
                 raster.levels.empty() ? 0U : raster.levels.back().width)},
            {QStringLiteral("coarsest_backed_level_width"),
             static_cast<qint64>(
                 rasterBackedLevelCount(raster.levels) == 0
                     ? 0U
                     : raster.levels[rasterBackedLevelCount(raster.levels) - 1]
                           .width)},
            {QStringLiteral("insufficient_overviews"),
             raster.insufficientOverviews},
            {QStringLiteral("crs_missing"), raster.crsMissing},
            {QStringLiteral("crs_mismatch"), raster.crsMismatch},
            {QStringLiteral("extent_disjoint"), raster.extentDisjointXY},
            {QStringLiteral("visible"), layer.visible},
        });
    }

    QJsonArray frames;
    for (std::size_t index = 0; index < frameMetrics_.size(); ++index) {
        const RenderMetrics &frame = frameMetrics_[index];
        const double frameMilliseconds = frame.sampledFrameMilliseconds > 0.0
                                             ? frame.sampledFrameMilliseconds
                                             : frame.frameMilliseconds;
        frames.append(QJsonObject{
            {QStringLiteral("sample_index"), static_cast<qint64>(index)},
            {QStringLiteral("submitted_frame"),
             static_cast<qint64>(frame.submittedFrameCount)},
            {QStringLiteral("timing_source"), frame.timingSource},
            {QStringLiteral("phase"), frame.qualificationPhase},
            {QStringLiteral("path_frame"),
             static_cast<qint64>(frame.qualificationFrameIndex)},
            {QStringLiteral("frame_ms"), frameMilliseconds},
            {QStringLiteral("gpu_frame_ms"),
             frame.timingSource == QStringLiteral("GPU")
                 ? QJsonValue(frameMilliseconds)
                 : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("output_width"), frame.outputWidth},
            {QStringLiteral("output_height"), frame.outputHeight},
            {QStringLiteral("source_points"),
             static_cast<qint64>(frame.sourcePoints)},
            {QStringLiteral("requested_points"),
             static_cast<qint64>(frame.requestedPoints)},
            {QStringLiteral("selected_points"),
             static_cast<qint64>(frame.selectedPoints)},
            {QStringLiteral("submitted_points"),
             static_cast<qint64>(frame.submittedPoints)},
            {QStringLiteral("draw_calls"),
             static_cast<qint64>(frame.drawCalls)},
            {QStringLiteral("visible_sources"),
             static_cast<qint64>(frame.visibleLayerCount)},
            {QStringLiteral("covered_sources"),
             static_cast<qint64>(frame.coveredLayerCount)},
            {QStringLiteral("root_only_sources"),
             static_cast<qint64>(frame.rootOnlyLayerCount)},
            {QStringLiteral("visible_blocks"),
             static_cast<qint64>(frame.visibleBlocks)},
            {QStringLiteral("culled_blocks"),
             static_cast<qint64>(frame.culledBlocks)},
            {QStringLiteral("decode_requests_queued"),
             static_cast<qint64>(frame.decodeRequestsQueued)},
            {QStringLiteral("decode_requests_started"),
             static_cast<qint64>(frame.decodeRequestsStarted)},
            {QStringLiteral("decode_requests_completed"),
             static_cast<qint64>(frame.decodeRequestsCompleted)},
            {QStringLiteral("uploaded_point_bytes"),
             static_cast<qint64>(frame.uploadedPointBytes)},
            {QStringLiteral("pending_upload_bytes"),
             static_cast<qint64>(frame.pendingUploadBytes)},
            {QStringLiteral("protected_gpu_point_bytes"),
             static_cast<qint64>(frame.protectedGpuPointBytes)},
            {QStringLiteral("gpu_resident_bytes"),
             static_cast<qint64>(frame.gpuPointBytes)},
            {QStringLiteral("gpu_budget_bytes"),
             static_cast<qint64>(frame.gpuPointBudgetBytes)},
            {QStringLiteral("gpu_evictions"),
             static_cast<qint64>(frame.gpuCacheEvictions)},
            {QStringLiteral("cpu_resident_bytes"),
             static_cast<qint64>(frame.decodedPointBytes)},
            {QStringLiteral("cpu_budget_bytes"),
             static_cast<qint64>(frame.decodedPointBudgetBytes)},
            {QStringLiteral("cpu_evictions"),
             static_cast<qint64>(frame.cacheEvictions)},
            {QStringLiteral("selection_ms"), frame.selectionMilliseconds},
            {QStringLiteral("command_recording_ms"),
             frame.commandRecordingMilliseconds},
            {QStringLiteral("included_uploads"), frame.frameIncludedUploads},
            {QStringLiteral("included_pick"), frame.frameIncludedPick},
        });
    }

    const QualificationEvaluation qualification =
        evaluateQualification(frameMetrics_);
    QJsonArray assertions;
    for (const QualificationAssertion &assertion : qualification.assertions) {
        assertions.append(QJsonObject{
            {QStringLiteral("id"), assertion.id},
            {QStringLiteral("status"),
             !assertion.evaluated ? QStringLiteral("not_evaluated")
             : assertion.passed   ? QStringLiteral("passed")
                                  : QStringLiteral("failed")},
            {QStringLiteral("observed_min"), assertion.observedMinimum},
            {QStringLiteral("observed_max"), assertion.observedMaximum},
            {QStringLiteral("required_min"), assertion.requiredMinimum},
            {QStringLiteral("required_max"), assertion.requiredMaximum},
            {QStringLiteral("sample_count"),
             static_cast<qint64>(assertion.sampleCount)},
            {QStringLiteral("unit"), assertion.unit},
            {QStringLiteral("details"), assertion.details},
        });
    }

    const RenderMetrics metrics = lastRenderMetrics_.value_or(RenderMetrics{});
    const PointCloudLoadControllerMetrics loadMetrics =
        session.pointLoadMetrics();
    const SceneSessionTimings sessionTimings = session.timings();
    const QJsonObject report{
        {QStringLiteral("schema"),
         QStringLiteral("pcinspector.release-h.native.v1")},
        {QStringLiteral("timestamp_utc"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("status"), status},
        {QStringLiteral("qualification_passed"), qualification.passed},
        {QStringLiteral("qualification_assertions"), assertions},
        {QStringLiteral("os"), QSysInfo::prettyProductName()},
        {QStringLiteral("cpu_architecture"),
         QSysInfo::currentCpuArchitecture()},
        {QStringLiteral("gpu_device"), metrics.deviceName},
        {QStringLiteral("requested_backend"), metrics.requestedBackend},
        {QStringLiteral("selected_backend"), metrics.selectedBackend},
        {QStringLiteral("gpu_validation"), metrics.gpuValidationEnabled},
        {QStringLiteral("source_count"),
         static_cast<qint64>(document->layerCount())},
        {QStringLiteral("first_any_preview_ms"),
         sessionTimings.timeToFirstPointsMilliseconds.value_or(0.0)},
        {QStringLiteral("first_all_preview_ms"),
         sessionTimings.timeToAllFirstPointsMilliseconds.value_or(
             sessionTimings.timeToFirstPointsMilliseconds.value_or(0.0))},
        {QStringLiteral("display_ready_ms"),
         sessionTimings.displayReadyMilliseconds.value_or(0.0)},
        {QStringLiteral("cpu_budget_bytes"),
         static_cast<qint64>(metrics.decodedPointBudgetBytes)},
        {QStringLiteral("cpu_resident_bytes"),
         static_cast<qint64>(metrics.decodedPointBytes)},
        {QStringLiteral("cpu_peak_bytes"),
         static_cast<qint64>(metrics.peakDecodedPointBytes)},
        {QStringLiteral("gpu_budget_bytes"),
         static_cast<qint64>(metrics.gpuPointBudgetBytes)},
        {QStringLiteral("gpu_resident_bytes"),
         static_cast<qint64>(metrics.gpuPointBytes)},
        {QStringLiteral("gpu_peak_bytes"),
         static_cast<qint64>(metrics.peakGpuPointBytes)},
        {QStringLiteral("persistent_index_bytes"),
         static_cast<qint64>(persistentBytes)},
        {QStringLiteral("source_file_bytes"),
         static_cast<qint64>(sourceFileBytes)},
        {QStringLiteral("rss_bytes"),
         static_cast<qint64>(metrics.processResidentBytes)},
        {QStringLiteral("rss_peak_bytes"),
         static_cast<qint64>(metrics.peakProcessResidentBytes)},
        {QStringLiteral("draw_calls"), static_cast<qint64>(metrics.drawCalls)},
        {QStringLiteral("source_points"),
         static_cast<qint64>(metrics.sourcePoints)},
        {QStringLiteral("submitted_points"),
         static_cast<qint64>(metrics.submittedPoints)},
        {QStringLiteral("visible_sources"),
         static_cast<qint64>(metrics.visibleLayerCount)},
        {QStringLiteral("covered_sources"),
         static_cast<qint64>(metrics.coveredLayerCount)},
        {QStringLiteral("frame_samples"),
         static_cast<qint64>(frameTimes.size())},
        {QStringLiteral("frame_ms_min"), percentile(0.0)},
        {QStringLiteral("frame_ms_p50"), percentile(0.5)},
        {QStringLiteral("frame_ms_p95"), percentile(0.95)},
        {QStringLiteral("frame_ms_max"), percentile(1.0)},
        {QStringLiteral("import_peak_workers"),
         static_cast<qint64>(loadMetrics.scheduler.peakActive)},
        {QStringLiteral("import_peak_queue"),
         static_cast<qint64>(loadMetrics.scheduler.peakPending)},
        {QStringLiteral("import_peak_bytes"),
         static_cast<qint64>(loadMetrics.scheduler.peakActiveEstimatedBytes)},
        {QStringLiteral("cache_hits"), static_cast<qint64>(metrics.cacheHits)},
        {QStringLiteral("cache_misses"),
         static_cast<qint64>(metrics.cacheMisses)},
        {QStringLiteral("cache_evictions"),
         static_cast<qint64>(metrics.cacheEvictions)},
        {QStringLiteral("decode_cancelled"),
         static_cast<qint64>(metrics.decodeRequestsCancelled)},
        {QStringLiteral("gdal_probed"), gdalRuntimeInfo_.probed},
        {QStringLiteral("gdal_version"), gdalRuntimeInfo_.version},
        // Driver availability is reported per driver rather than as one
        // capability flag, so a failure report distinguishes a missing
        // container from a missing index format.
        {QStringLiteral("gdal_driver_gti"), gdalRuntimeInfo_.tileIndexDriver},
        {QStringLiteral("gdal_driver_vrt"),
         gdalRuntimeInfo_.virtualRasterDriver},
        {QStringLiteral("gdal_driver_gpkg"), gdalRuntimeInfo_.geoPackageDriver},
        {QStringLiteral("gdal_driver_flatgeobuf"),
         gdalRuntimeInfo_.flatGeobufDriver},
        {QStringLiteral("gdal_driver_shapefile"),
         gdalRuntimeInfo_.shapefileDriver},
        {QStringLiteral("catalog_import_available"),
         gdalRuntimeInfo_.catalogImport()},
        {QStringLiteral("raster_cpu_bytes"),
         static_cast<qint64>(metrics.rasterCpuBytes)},
        {QStringLiteral("raster_cpu_budget_bytes"),
         static_cast<qint64>(metrics.rasterCpuBudgetBytes)},
        {QStringLiteral("raster_cpu_peak_bytes"),
         static_cast<qint64>(metrics.rasterCpuPeakBytes)},
        {QStringLiteral("raster_gpu_bytes"),
         static_cast<qint64>(metrics.rasterGpuBytes)},
        {QStringLiteral("raster_height_gpu_bytes"),
         static_cast<qint64>(metrics.rasterHeightGpuBytes)},
        {QStringLiteral("raster_gpu_budget_bytes"),
         static_cast<qint64>(metrics.rasterGpuBudgetBytes)},
        {QStringLiteral("raster_gpu_peak_bytes"),
         static_cast<qint64>(metrics.rasterGpuPeakBytes)},
        {QStringLiteral("gdal_cache_budget_bytes"),
         static_cast<qint64>(gdalCache.budgetBytes)},
        {QStringLiteral("gdal_cache_used_bytes"),
         static_cast<qint64>(gdalCache.usedBytes.value_or(0))},
        {QStringLiteral("raster_tiles_requested"),
         static_cast<qint64>(metrics.rasterTilesRequested)},
        {QStringLiteral("raster_tiles_completed"),
         static_cast<qint64>(metrics.rasterTilesCompleted)},
        {QStringLiteral("raster_tiles_failed"),
         static_cast<qint64>(metrics.rasterTilesFailed)},
        {QStringLiteral("raster_surface_drawn_tiles"),
         static_cast<qint64>(metrics.rasterSurfaceDrawnTiles)},
        {QStringLiteral("raster_surface_triangles"),
         static_cast<qint64>(metrics.rasterSurfaceTriangles)},
        {QStringLiteral("raster_tiles_resident"),
         static_cast<qint64>(metrics.rasterResidentTiles)},
        {QStringLiteral("raster_finest_level"),
         static_cast<qint64>(metrics.rasterFinestLevel)},
        {QStringLiteral("raster_coarsest_level"),
         static_cast<qint64>(metrics.rasterCoarsestLevel)},
        {QStringLiteral("raster_coverage_incomplete"),
         metrics.rasterCoverageIncomplete},
        {QStringLiteral("frames"), frames},
        {QStringLiteral("sources"), sources},
        {QStringLiteral("layers"), layers},
        {QStringLiteral("raster_layers"), rasterLayers},
    };

    QualificationWriteResult result{
        .written = false,
        .exitRequested = exitAfterWrite_,
        .qualificationPassed = qualification.passed,
        .filename = pathToQString(outputPath_),
        .error = {},
    };
    QDir().mkpath(QFileInfo(result.filename).absolutePath());
    QSaveFile output(result.filename);
    if (!output.open(QIODevice::WriteOnly)) {
        reportWritten_ = false;
        result.error =
            QStringLiteral("Could not write qualification report: %1")
                .arg(output.errorString());
        return result;
    }
    output.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    if (!output.commit()) {
        reportWritten_ = false;
        result.error =
            QStringLiteral("Could not commit qualification report: %1")
                .arg(output.errorString());
        return result;
    }
    result.written = true;
    return result;
}

} // namespace pci
