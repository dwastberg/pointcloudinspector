#include "app/QualificationCriteria.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {

pci::RenderMetrics frame(const std::uint32_t index,
                         const std::uint32_t count,
                         const QString &phase)
{
    return {
        .timingSource = QStringLiteral("GPU"),
        .sampledFrameMilliseconds = 10.0,
        .outputWidth = 100,
        .outputHeight = 100,
        .qualificationPhase = phase,
        .qualificationFrameIndex = index,
        .qualificationFrameCount = count,
        .qualificationFrame = true,
        .qualificationFinalFrame = index + 1U == count,
        .submittedPoints = 20'000,
        .uploadedPointBytes = 0,
        .pendingUploadBytes = 0,
        .selectionMilliseconds = 1.0,
        .gpuPointBudgetBytes = 1'000,
        .decodedPointBytes = 500,
        .decodedPointBudgetBytes = 1'000,
        .gpuPointBytes = 500,
        .gpuCacheEvictions = 2,
        .cacheEvictions = 3,
        .decodeRequestsStarted = 4,
        .visibleLayerCount = 6,
        .coveredLayerCount = 6,
        .rootOnlyLayerCount = 0,
    };
}

std::vector<pci::RenderMetrics> passingFrames()
{
    constexpr std::uint32_t count = 40;
    std::vector<pci::RenderMetrics> result;
    result.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const QString phase = index < 2   ? QStringLiteral("warmup")
                              : index < 8 ? QStringLiteral("interacting")
                                          : QStringLiteral("dwell");
        result.push_back(frame(index, count, phase));
    }
    return result;
}

const pci::QualificationAssertion &
assertion(const pci::QualificationEvaluation &evaluation,
          const std::string_view id)
{
    const auto found = std::ranges::find(evaluation.assertions,
                                         QString::fromUtf8(id),
                                         &pci::QualificationAssertion::id);
    REQUIRE(found != evaluation.assertions.end());
    return *found;
}

TEST_CASE("qualification criteria accept a bounded settled replay",
          "[unit][qualification][criteria]")
{
    const pci::QualificationEvaluation evaluation =
        pci::evaluateQualification(passingFrames());

    CHECK(evaluation.passed);
    CHECK(std::ranges::all_of(evaluation.assertions, [](const auto &entry) {
        return !entry.evaluated || entry.passed;
    }));
    CHECK(assertion(evaluation, "camera_path_complete").sampleCount == 40);
    CHECK(assertion(evaluation, "settled_interacting_point_density")
              .observedMaximum == 2.0);
    CHECK(assertion(evaluation, "stationary_working_set_stable").passed);
    CHECK(assertion(evaluation, "stationary_uploads_settle").passed);
}

TEST_CASE("qualification criteria identify independent renderer regressions",
          "[unit][qualification][criteria]")
{
    std::vector<pci::RenderMetrics> frames = passingFrames();
    frames.erase(frames.begin() + 1);
    for (pci::RenderMetrics &sample : frames) {
        if (sample.qualificationPhase == QStringLiteral("interacting")) {
            sample.sampledFrameMilliseconds = 20.0;
            sample.submittedPoints = 5'000;
        }
    }
    pci::RenderMetrics &interaction = frames[2];
    interaction.coveredLayerCount = 5;
    interaction.rootOnlyLayerCount = 1;
    interaction.uploadedPointBytes = 49ULL * 1024ULL * 1024ULL;
    interaction.selectionMilliseconds = 2.1;
    interaction.decodedPointBytes = 1'001;
    interaction.gpuPointBytes = 1'001;
    frames.back().cacheEvictions = 4;
    frames.back().decodeRequestsStarted = 5;
    frames.back().uploadedPointBytes = 1;

    const pci::QualificationEvaluation evaluation =
        pci::evaluateQualification(frames);
    CHECK_FALSE(evaluation.passed);
    for (const std::string_view id : {
             "camera_path_complete",
             "all_visible_sources_represented",
             "no_roots_only_frames",
             "point_upload_bytes_per_frame",
             "decoded_cpu_cache_ceiling",
             "gpu_point_cache_ceiling",
             "selection_time",
             "interacting_frame_time_p95",
             "settled_interacting_point_density",
             "stationary_working_set_stable",
             "stationary_uploads_settle",
         }) {
        INFO("assertion: " << id);
        const pci::QualificationAssertion &entry = assertion(evaluation, id);
        CHECK(entry.evaluated);
        CHECK_FALSE(entry.passed);
    }
}

TEST_CASE("qualification criteria do not invent unavailable measurements",
          "[unit][qualification][criteria]")
{
    const pci::QualificationEvaluation evaluation =
        pci::evaluateQualification({});
    CHECK(evaluation.passed);
    CHECK(std::ranges::all_of(evaluation.assertions, [](const auto &entry) {
        return !entry.evaluated;
    }));
}

} // namespace
