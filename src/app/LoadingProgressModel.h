#pragma once

#include "import/PointCloudImport.h"
#include "renderer/RenderLoadProgress.h"

#include <chrono>
#include <cstdint>
#include <optional>

namespace pci {

enum class LoadingProgressPhase {
    Reading,
    Optimizing,
    PreparingRenderer,
    Uploading,
    FirstFrameReady,
    FullDetailWarming,
    DisplayReady,
};

struct LoadingProgressState {
    int percentage = 0;
    LoadingProgressPhase phase = LoadingProgressPhase::Reading;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
    std::uint64_t decoded = 0;
    std::uint64_t uploaded = 0;
    bool estimated = false;
};

class LoadingProgressModel {
public:
    void reset() noexcept;
    void setPagedSource(bool paged) noexcept;

    [[nodiscard]] LoadingProgressState state() const noexcept;
    [[nodiscard]] LoadingProgressState completeImport() noexcept;
    [[nodiscard]] LoadingProgressState
    updateImport(PointCloudImportStage stage,
                 std::uint64_t processed,
                 std::uint64_t total) noexcept;
    [[nodiscard]] LoadingProgressState
    updateRender(RenderLoadProgress progress) noexcept;
    [[nodiscard]] LoadingProgressState
    advance(std::chrono::milliseconds elapsed) noexcept;

private:
    void beginEstimate(LoadingProgressPhase phase,
                       int startPercent,
                       int endPercent,
                       std::chrono::milliseconds estimate) noexcept;
    void setPercentage(int percentage) noexcept;

    LoadingProgressState state_;
    bool importReadingComplete_ = false;
    bool pagedSource_ = false;
    bool incrementalImportProgress_ = false;
    std::optional<RenderLoadProgress> deferredRenderProgress_;
    int estimateStartPercent_ = 0;
    int estimateEndPercent_ = 0;
    std::chrono::milliseconds estimateDuration_{1};
};

} // namespace pci
