#include "app/QualificationReporter.h"

#include "app/SceneSession.h"
#include "foundation/CheckedArithmetic.h"
#include "platform/QtPath.h"

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
    frameMilliseconds_.clear();
}

bool QualificationReporter::configured() const noexcept
{
    return !outputPath_.empty();
}

void QualificationReporter::record(const RenderMetrics &metrics)
{
    lastRenderMetrics_ = metrics;
    if (configured() && metrics.frameMilliseconds > 0.0 &&
        frameMilliseconds_.size() < 16'384) {
        frameMilliseconds_.push_back(metrics.frameMilliseconds);
    }
}

QualificationWriteResult QualificationReporter::write(const QString &status,
                                                      SceneSession &session)
{
    if (!configured() || reportWritten_) {
        return {};
    }
    reportWritten_ = true;

    std::vector<double> frameTimes = frameMilliseconds_;
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
            {QStringLiteral("error"), state.detail},
        });
    }

    QJsonArray layers;
    std::uint64_t persistentBytes = 0;
    std::uint64_t sourceFileBytes = 0;
    for (const PointCloudLayer &layer : session.document()->layers()) {
        const PointCloudStorageMetrics storage = layer.scene->storageMetrics();
        persistentBytes =
            saturatingAdd(persistentBytes, storage.persistentBytes);
        std::error_code fileSizeError;
        const std::uintmax_t layerFileBytes = std::filesystem::file_size(
            layer.scene->metadata().sourcePath, fileSizeError);
        const std::uint64_t boundedLayerFileBytes =
            fileSizeError ? 0
            : layerFileBytes > std::numeric_limits<std::uint64_t>::max()
                ? std::numeric_limits<std::uint64_t>::max()
                : static_cast<std::uint64_t>(layerFileBytes);
        sourceFileBytes = saturatingAdd(sourceFileBytes, boundedLayerFileBytes);
        layers.append(QJsonObject{
            {QStringLiteral("layer_id"), static_cast<qint64>(layer.id.value())},
            {QStringLiteral("path"),
             pathToQString(layer.scene->metadata().sourcePath)},
            {QStringLiteral("source_points"),
             static_cast<qint64>(layer.scene->metadata().sourcePointCount)},
            {QStringLiteral("source_file_bytes"),
             static_cast<qint64>(boundedLayerFileBytes)},
            {QStringLiteral("index_bytes"),
             static_cast<qint64>(storage.persistentBytes)},
            {QStringLiteral("index_reused"), storage.reused},
            {QStringLiteral("visible"), layer.visible},
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
        {QStringLiteral("os"), QSysInfo::prettyProductName()},
        {QStringLiteral("cpu_architecture"),
         QSysInfo::currentCpuArchitecture()},
        {QStringLiteral("gpu_device"), metrics.deviceName},
        {QStringLiteral("requested_backend"), metrics.requestedBackend},
        {QStringLiteral("selected_backend"), metrics.selectedBackend},
        {QStringLiteral("gpu_validation"), metrics.gpuValidationEnabled},
        {QStringLiteral("source_count"),
         static_cast<qint64>(session.document()->layerCount())},
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
        {QStringLiteral("full_detail_active"), metrics.fullDetailActive},
        {QStringLiteral("full_detail_decoded_nodes"),
         static_cast<qint64>(metrics.fullDetailDecodedNodes)},
        {QStringLiteral("full_detail_total_nodes"),
         static_cast<qint64>(metrics.fullDetailTotalNodes)},
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
        {QStringLiteral("sources"), sources},
        {QStringLiteral("layers"), layers},
    };

    QualificationWriteResult result{
        .exitRequested = exitAfterWrite_,
        .filename = pathToQString(outputPath_),
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
