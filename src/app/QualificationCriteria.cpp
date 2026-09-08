#include "app/QualificationCriteria.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ranges>
#include <vector>

namespace pci {
namespace {

constexpr double maximumUploadBytes = 48.0 * 1024.0 * 1024.0;
constexpr double maximumSelectionMilliseconds = 2.0;
constexpr double maximumInteractingP95Milliseconds = 16.7;
constexpr double minimumPointPixelRatio = 1.0;
constexpr double maximumPointPixelRatio = 4.0;

bool measured(const RenderMetrics &frame) noexcept
{
    return frame.qualificationFrame &&
           frame.qualificationPhase != QStringLiteral("warmup");
}

double exactFrameMilliseconds(const RenderMetrics &frame) noexcept
{
    return frame.sampledFrameMilliseconds > 0.0 ? frame.sampledFrameMilliseconds
                                                : frame.frameMilliseconds;
}

double percentile(std::vector<double> values, const double fraction)
{
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    const std::size_t index = static_cast<std::size_t>(
        std::clamp(fraction * static_cast<double>(values.size() - 1U),
                   0.0,
                   static_cast<double>(values.size() - 1U)));
    return values[index];
}

QualificationAssertion upperBound(QString id,
                                  const std::span<const double> values,
                                  const double limit,
                                  QString unit,
                                  QString details)
{
    QualificationAssertion result{
        .id = std::move(id),
        .evaluated = !values.empty(),
        .passed = false,
        .requiredMaximum = limit,
        .sampleCount = values.size(),
        .unit = std::move(unit),
        .details = std::move(details),
    };
    if (values.empty()) {
        return result;
    }
    const auto [minimum, maximum] = std::ranges::minmax(values);
    result.observedMinimum = minimum;
    result.observedMaximum = maximum;
    result.passed = maximum <= limit;
    return result;
}

void append(QualificationEvaluation &evaluation,
            QualificationAssertion assertion)
{
    if (assertion.evaluated && !assertion.passed) {
        evaluation.passed = false;
    }
    evaluation.assertions.push_back(std::move(assertion));
}

std::vector<const RenderMetrics *>
phaseFrames(const std::span<const RenderMetrics> frames, const QString &phase)
{
    std::vector<const RenderMetrics *> result;
    for (const RenderMetrics &frame : frames) {
        if (frame.qualificationFrame && frame.qualificationPhase == phase) {
            result.push_back(&frame);
        }
    }
    return result;
}

} // namespace

QualificationEvaluation
evaluateQualification(const std::span<const RenderMetrics> frames)
{
    QualificationEvaluation evaluation;
    const std::vector<const RenderMetrics *> replay = [&frames] {
        std::vector<const RenderMetrics *> result;
        for (const RenderMetrics &frame : frames) {
            if (frame.qualificationFrame) {
                result.push_back(&frame);
            }
        }
        return result;
    }();

    QualificationAssertion complete{
        .id = QStringLiteral("camera_path_complete"),
        .evaluated = !replay.empty(),
        .sampleCount = replay.size(),
        .unit = QStringLiteral("frames"),
        .details = QStringLiteral(
            "Every frame-indexed camera-path sample is present exactly once."),
    };
    if (!replay.empty()) {
        const std::uint32_t expected = replay.front()->qualificationFrameCount;
        complete.observedMinimum = static_cast<double>(replay.size());
        complete.observedMaximum = static_cast<double>(replay.size());
        complete.requiredMinimum = expected;
        complete.requiredMaximum = expected;
        complete.passed = expected > 0 && replay.size() == expected &&
                          replay.back()->qualificationFinalFrame;
        for (std::size_t index = 0; index < replay.size() && complete.passed;
             ++index) {
            complete.passed =
                replay[index]->qualificationFrameIndex == index &&
                replay[index]->qualificationFrameCount == expected;
        }
    }
    append(evaluation, std::move(complete));

    std::vector<double> coverage;
    std::vector<double> rootsOnly;
    std::vector<double> uploads;
    std::vector<double> selectionTimes;
    std::vector<double> cpuUtilization;
    std::vector<double> gpuUtilization;
    for (const RenderMetrics &frame : frames) {
        if (!measured(frame)) {
            continue;
        }
        if (frame.visibleLayerCount > 0) {
            coverage.push_back(static_cast<double>(frame.coveredLayerCount) /
                               static_cast<double>(frame.visibleLayerCount));
            rootsOnly.push_back(static_cast<double>(frame.rootOnlyLayerCount));
        }
        uploads.push_back(static_cast<double>(frame.uploadedPointBytes));
        selectionTimes.push_back(frame.selectionMilliseconds);
        if (frame.decodedPointBudgetBytes > 0) {
            cpuUtilization.push_back(
                static_cast<double>(frame.decodedPointBytes) /
                static_cast<double>(frame.decodedPointBudgetBytes));
        }
        if (frame.gpuPointBudgetBytes > 0) {
            gpuUtilization.push_back(
                static_cast<double>(frame.gpuPointBytes) /
                static_cast<double>(frame.gpuPointBudgetBytes));
        }
    }

    QualificationAssertion represented = upperBound(
        QStringLiteral("all_visible_sources_represented"),
        coverage,
        1.0,
        QStringLiteral("covered/visible"),
        QStringLiteral("Every in-frustum source contributes a draw."));
    if (represented.evaluated) {
        represented.passed = represented.observedMinimum >= 1.0 &&
                             represented.observedMaximum <= 1.0;
        represented.requiredMinimum = 1.0;
    }
    append(evaluation, std::move(represented));
    append(evaluation,
           upperBound(QStringLiteral("no_roots_only_frames"),
                      rootsOnly,
                      0.0,
                      QStringLiteral("layers"),
                      QStringLiteral("No visible hierarchy falls back to only "
                                     "its root after warm-up.")));
    append(
        evaluation,
        upperBound(QStringLiteral("point_upload_bytes_per_frame"),
                   uploads,
                   maximumUploadBytes,
                   QStringLiteral("bytes"),
                   QStringLiteral("Point uploads stay at or below 48 MiB.")));
    append(
        evaluation,
        upperBound(QStringLiteral("decoded_cpu_cache_ceiling"),
                   cpuUtilization,
                   1.0,
                   QStringLiteral("resident/budget"),
                   QStringLiteral("Decoded point pages stay inside budget.")));
    append(evaluation,
           upperBound(QStringLiteral("gpu_point_cache_ceiling"),
                      gpuUtilization,
                      1.0,
                      QStringLiteral("resident/budget"),
                      QStringLiteral("GPU point buffers stay inside budget.")));
    append(evaluation,
           upperBound(QStringLiteral("selection_time"),
                      selectionTimes,
                      maximumSelectionMilliseconds,
                      QStringLiteral("ms"),
                      QStringLiteral("Frame selection stays below 2 ms.")));

    const std::vector<const RenderMetrics *> interacting =
        phaseFrames(frames, QStringLiteral("interacting"));
    std::vector<double> interactionTimes;
    for (const RenderMetrics *frame : interacting) {
        const double value = exactFrameMilliseconds(*frame);
        if (value > 0.0 && std::isfinite(value)) {
            interactionTimes.push_back(value);
        }
    }
    QualificationAssertion p95{
        .id = QStringLiteral("interacting_frame_time_p95"),
        .evaluated = !interactionTimes.empty(),
        .requiredMaximum = maximumInteractingP95Milliseconds,
        .sampleCount = interactionTimes.size(),
        .unit = QStringLiteral("ms"),
        .details =
            QStringLiteral("Warm interacting p95 targets one 60 Hz frame."),
    };
    if (!interactionTimes.empty()) {
        const double value = percentile(interactionTimes, 0.95);
        p95.observedMinimum = value;
        p95.observedMaximum = value;
        p95.passed = value <= maximumInteractingP95Milliseconds;
    }
    append(evaluation, std::move(p95));

    std::vector<double> density;
    const std::size_t densityStart = interacting.size() * 2U / 3U;
    for (std::size_t index = densityStart; index < interacting.size();
         ++index) {
        const RenderMetrics &frame = *interacting[index];
        if (frame.outputWidth <= 0 || frame.outputHeight <= 0 ||
            frame.visibleLayerCount == 0) {
            continue;
        }
        const double pixels = static_cast<double>(frame.outputWidth) *
                              static_cast<double>(frame.outputHeight);
        density.push_back(static_cast<double>(frame.submittedPoints) / pixels);
    }
    QualificationAssertion pointDensity{
        .id = QStringLiteral("settled_interacting_point_density"),
        .evaluated = !density.empty(),
        .requiredMinimum = minimumPointPixelRatio,
        .requiredMaximum = maximumPointPixelRatio,
        .sampleCount = density.size(),
        .unit = QStringLiteral("points/pixel"),
        .details = QStringLiteral(
            "The settled interaction median stays between 1x and 4x viewport "
            "pixels."),
    };
    if (!density.empty()) {
        const double median = percentile(density, 0.5);
        pointDensity.observedMinimum = median;
        pointDensity.observedMaximum = median;
        pointDensity.passed = median >= minimumPointPixelRatio &&
                              median <= maximumPointPixelRatio;
    }
    append(evaluation, std::move(pointDensity));

    const std::vector<const RenderMetrics *> dwell =
        phaseFrames(frames, QStringLiteral("dwell"));
    QualificationAssertion stableWorkingSet{
        .id = QStringLiteral("stationary_working_set_stable"),
        .evaluated = dwell.size() >= 4,
        .requiredMaximum = 0.0,
        .sampleCount = dwell.size() / 2U,
        .unit = QStringLiteral("counter increments"),
        .details = QStringLiteral(
            "CPU/GPU evictions and decode starts stop in the second half of "
            "the stationary dwell."),
    };
    if (stableWorkingSet.evaluated) {
        const std::size_t start = dwell.size() / 2U;
        const RenderMetrics &first = *dwell[start];
        const RenderMetrics &last = *dwell.back();
        const auto increment = [](const std::uint64_t from,
                                  const std::uint64_t to) {
            return to >= from ? to - from
                              : std::numeric_limits<std::uint64_t>::max();
        };
        const std::uint64_t changes = std::max({
            increment(first.cacheEvictions, last.cacheEvictions),
            increment(first.gpuCacheEvictions, last.gpuCacheEvictions),
            increment(first.decodeRequestsStarted, last.decodeRequestsStarted),
        });
        stableWorkingSet.observedMinimum = static_cast<double>(changes);
        stableWorkingSet.observedMaximum = static_cast<double>(changes);
        stableWorkingSet.passed = changes == 0;
    }
    append(evaluation, std::move(stableWorkingSet));

    QualificationAssertion settledUploads{
        .id = QStringLiteral("stationary_uploads_settle"),
        .evaluated = dwell.size() >= 30,
        .requiredMaximum = 0.0,
        .sampleCount = std::min<std::size_t>(30, dwell.size()),
        .unit = QStringLiteral("bytes"),
        .details = QStringLiteral(
            "No point uploads remain in the final 30 stationary frames."),
    };
    if (settledUploads.evaluated) {
        double maximum = 0.0;
        for (std::size_t index = dwell.size() - 30U; index < dwell.size();
             ++index) {
            maximum = std::max(
                maximum, static_cast<double>(dwell[index]->uploadedPointBytes));
            maximum = std::max(
                maximum, static_cast<double>(dwell[index]->pendingUploadBytes));
        }
        settledUploads.observedMinimum = maximum;
        settledUploads.observedMaximum = maximum;
        settledUploads.passed = maximum == 0.0;
    }
    append(evaluation, std::move(settledUploads));

    return evaluation;
}

} // namespace pci
