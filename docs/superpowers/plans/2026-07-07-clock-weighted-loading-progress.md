# Clock-Weighted Loading Progress Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make loading progress percentages match observed wall-clock time more closely, with PDAL reaching about 33% instead of 100%, and 100% shown only when the new point cloud is ready to display.

**Architecture:** Replace raw `processed / total` UI progress with staged progress. The import worker reports semantic stages (`Reading`, `Optimizing`), `MainWindow` maps those stages onto measured UI percentage ranges, and the renderer reports upload/first-frame readiness so the overlay only completes after the new cloud has rendered. Stage ranges are fixed for this first pass: reading 0–33%, optimizing 33–80%, renderer upload/preparation 80–98%, first frame ready 98–100%.

**Tech Stack:** C++23, Qt 6 Widgets, Qt Concurrent, QRhi/Metal, CMake/CTest.

---

## File structure

- Modify `src/import/PointCloudImport.h`: define `PointCloudImportStage`, `PointCloudImportProgress`, and change the import progress callback to carry stage metadata.
- Modify `src/import/pdal/PdalPointCloudLoader.cpp`: emit `Reading` progress during PDAL streaming and `Optimizing` before the post-stream sort.
- Modify `src/import/PointCloudLoadController.{h,cpp}`: forward staged progress through Qt signals.
- Modify `src/renderer/RenderViewport.h`: add renderer load progress types and a callback setter.
- Modify `src/renderer/metal/MetalRenderViewport.{h,cpp}`: report renderer upload progress and first-frame readiness for newly loaded point clouds.
- Modify `src/app/LoadingOverlay.{h,cpp}`: add a direct percentage/details API so UI percentages can be controlled by staged mapping.
- Modify `src/app/MainWindow.{h,cpp}`: map import/render stages into clock-weighted percentages and keep the overlay visible until renderer readiness.
- Modify tests:
  - `tests/load_controller_tests.cpp`
  - `tests/loading_overlay_tests.cpp`
  - `tests/main_window_tests.cpp`
  - `tests/renderer_contract_tests.cpp`
  - `tests/metal_point_picker_tests.cpp`

---

## Progress mapping contract

Use these constants in `MainWindow.cpp`:

```cpp
namespace {

constexpr int readingStartPercent = 0;
constexpr int readingEndPercent = 33;
constexpr int optimizingStartPercent = 33;
constexpr int optimizingEndPercent = 80;
constexpr int rendererStartPercent = 80;
constexpr int rendererEndPercent = 98;
constexpr int firstFrameReadyPercent = 100;

constexpr int optimizingEstimateMilliseconds = 4200;
constexpr int rendererEstimateMilliseconds = 1600;

} // namespace
```

Rules:

- `Reading` maps `processed / total` into `0..33`.
- `Optimizing` starts at `33` and advances on a timer toward `79`; completion of the worker advances to renderer preparation.
- Renderer upload maps chunk progress into `80..98`.
- If renderer upload progress is not available yet, renderer preparation advances on a timer toward `97`.
- `100` is set only after renderer reports `FirstFrameReady`.
- The overlay hides only after `FirstFrameReady`, failure, or cancellation.

---

## Task 1: Add staged import progress

**Files:**
- Modify: `src/import/PointCloudImport.h`
- Modify: `src/import/pdal/PdalPointCloudLoader.cpp`
- Modify: `src/import/PointCloudLoadController.h`
- Modify: `src/import/PointCloudLoadController.cpp`
- Modify: `tests/load_controller_tests.cpp`

- [ ] **Step 1: Write failing load-controller stage test**

Modify `tests/load_controller_tests.cpp`:

1. Update `FakeLoader::load()` success progress emissions:

```cpp
        if (request.progress) {
            request.progress({
                .stage = pci::PointCloudImportStage::Reading,
                .processed = 0,
                .total = 2,
            });
            request.progress({
                .stage = pci::PointCloudImportStage::Reading,
                .processed = 2,
                .total = 2,
            });
            request.progress({
                .stage = pci::PointCloudImportStage::Optimizing,
                .processed = 2,
                .total = 2,
            });
        }
```

2. In `testSuccessRunsOffThreadAndSignalsOnOwnerThread()`, replace:

```cpp
    CHECK(progress.count() == 2);
```

with:

```cpp
    CHECK(progress.count() == 3);
    CHECK(qvariant_cast<pci::PointCloudImportStage>(
              progress.at(0).at(0))
          == pci::PointCloudImportStage::Reading);
    CHECK(progress.at(1).at(1).toULongLong() == 2);
    CHECK(progress.at(1).at(2).toULongLong() == 2);
    CHECK(qvariant_cast<pci::PointCloudImportStage>(
              progress.at(2).at(0))
          == pci::PointCloudImportStage::Optimizing);
```

3. In `main()`, register the enum:

```cpp
    qRegisterMetaType<pci::PointCloudImportStage>();
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build --target pcinspector_load_controller_tests
ctest --test-dir build --output-on-failure -R '^load_controller$'
```

Expected: build fails because `PointCloudImportStage` and the new progress callback shape do not exist.

- [ ] **Step 3: Add staged progress types**

Modify `src/import/PointCloudImport.h`:

```cpp
namespace pci {

enum class PointCloudImportStage {
    Reading,
    Optimizing,
};

struct PointCloudImportProgress {
    PointCloudImportStage stage = PointCloudImportStage::Reading;
    std::uint64_t processed = 0;
    std::uint64_t total = 0;
};

struct PointCloudImportRequest {
    std::filesystem::path sourcePath;
    std::uint64_t maximumPoints = 10'000'000;
    std::stop_token stopToken;
    std::function<void(PointCloudImportProgress)> progress;
};
```

Add after the namespace closes:

```cpp
Q_DECLARE_METATYPE(pci::PointCloudImportStage)
```

Add `#include <QMetaType>` at the top of `PointCloudImport.h`.

- [ ] **Step 4: Update PDAL loader progress emissions**

Modify `src/import/pdal/PdalPointCloudLoader.cpp`:

```cpp
    if (request.progress) {
        request.progress({
            .stage = PointCloudImportStage::Reading,
            .processed = 0,
            .total = metadata.sourcePointCount,
        });
    }
```

Inside the stream callback:

```cpp
            if (request.progress && processed % 65'536 == 0) {
                request.progress({
                    .stage = PointCloudImportStage::Reading,
                    .processed = processed,
                    .total = metadata.sourcePointCount,
                });
            }
```

After `callback.execute(table)`:

```cpp
        if (request.progress) {
            request.progress({
                .stage = PointCloudImportStage::Reading,
                .processed = processed,
                .total = metadata.sourcePointCount,
            });
        }
```

Immediately before `std::sort(...)`:

```cpp
    if (request.progress) {
        request.progress({
            .stage = PointCloudImportStage::Optimizing,
            .processed = cloud->points.size(),
            .total = cloud->points.size(),
        });
    }
```

- [ ] **Step 5: Forward staged progress through the controller**

Modify `src/import/PointCloudLoadController.h` signal:

```cpp
    void progressChanged(
        pci::PointCloudImportStage stage,
        quint64 processed,
        quint64 total);
```

Modify constructor in `src/import/PointCloudLoadController.cpp`:

```cpp
    qRegisterMetaType<PointCloudImportStage>();
```

Modify the progress lambda in `PointCloudLoadController::load()`:

```cpp
    request.progress =
        [guardedThis, jobId](const PointCloudImportProgress progress) {
            if (!guardedThis) {
                return;
            }
            QMetaObject::invokeMethod(
                guardedThis,
                [guardedThis, jobId, progress] {
                    if (guardedThis
                        && guardedThis->currentJobId_ == jobId) {
                        emit guardedThis->progressChanged(
                            progress.stage,
                            progress.processed,
                            progress.total);
                    }
                },
                Qt::QueuedConnection);
        };
```

- [ ] **Step 6: Run load-controller test**

Run:

```bash
cmake --build build --target pcinspector_load_controller_tests
ctest --test-dir build --output-on-failure -R '^load_controller$'
```

Expected: `load_controller` passes.

- [ ] **Step 7: Commit staged import progress**

```bash
git add src/import/PointCloudImport.h src/import/pdal/PdalPointCloudLoader.cpp src/import/PointCloudLoadController.h src/import/PointCloudLoadController.cpp tests/load_controller_tests.cpp
git commit -m "feat: add staged point cloud import progress"
```

---

## Task 2: Add direct percentage updates to LoadingOverlay

**Files:**
- Modify: `src/app/LoadingOverlay.h`
- Modify: `src/app/LoadingOverlay.cpp`
- Modify: `tests/loading_overlay_tests.cpp`

- [ ] **Step 1: Write failing overlay percentage test**

Add to `tests/loading_overlay_tests.cpp`:

```cpp
void testDirectProgressPercentageControlsDisplayedPercent()
{
    pci::LoadingOverlay overlay;

    overlay.showLoading(QStringLiteral("fixture.las"));
    overlay.setProgress(33, QStringLiteral("Optimizing points…"));

    auto *details =
        requiredChild<QLabel>(overlay, QStringLiteral("loadingDetailsLabel"));
    auto *bar =
        requiredChild<QProgressBar>(overlay, QStringLiteral("loadingProgressBar"));

    CHECK(bar->minimum() == 0);
    CHECK(bar->maximum() == 100);
    CHECK(bar->value() == 33);
    CHECK(details->text() == QStringLiteral("Optimizing points…"));
}
```

Call it from `main()`:

```cpp
        testDirectProgressPercentageControlsDisplayedPercent();
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build --target pcinspector_loading_overlay_tests
ctest --test-dir build --output-on-failure -R '^loading_overlay$'
```

Expected: build fails because `LoadingOverlay::setProgress()` does not exist.

- [ ] **Step 3: Add `setProgress()` API**

Modify `src/app/LoadingOverlay.h`:

```cpp
    void setProgress(int percentage, const QString &details);
```

Modify `src/app/LoadingOverlay.cpp`:

```cpp
void LoadingOverlay::setProgress(const int percentage, const QString &details)
{
    const int clamped = std::clamp(percentage, 0, 100);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(clamped);
    detailsLabel_->setText(details);
}
```

Leave `updateProgress(processed, total)` in place for now; `MainWindow` will switch to `setProgress()` in Task 4.

- [ ] **Step 4: Run overlay test**

Run:

```bash
cmake --build build --target pcinspector_loading_overlay_tests
ctest --test-dir build --output-on-failure -R '^loading_overlay$'
```

Expected: `loading_overlay` passes.

- [ ] **Step 5: Commit overlay direct progress API**

```bash
git add src/app/LoadingOverlay.h src/app/LoadingOverlay.cpp tests/loading_overlay_tests.cpp
git commit -m "feat: allow mapped loading overlay progress"
```

---

## Task 3: Add renderer readiness progress callbacks

**Files:**
- Modify: `src/renderer/RenderViewport.h`
- Modify: `src/renderer/metal/MetalRenderViewport.h`
- Modify: `src/renderer/metal/MetalRenderViewport.cpp`
- Modify: `tests/renderer_contract_tests.cpp`
- Modify: `tests/metal_point_picker_tests.cpp`
- Modify: `tests/main_window_tests.cpp`

- [ ] **Step 1: Write failing renderer contract test**

Modify `tests/renderer_contract_tests.cpp` before `viewport->setPointCloud(loaded);`:

```cpp
        bool sawRendererPreparing = false;
        viewport->setLoadProgressCallback(
            [&sawRendererPreparing](const pci::RenderLoadProgress &progress) {
                sawRendererPreparing =
                    progress.stage == pci::RenderLoadStage::Preparing;
            });
```

After `viewport->setPointCloud(loaded);` add:

```cpp
        CHECK(sawRendererPreparing);
```

This test verifies the interface-level callback is invoked when a new point cloud is accepted by the renderer.

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build --target pcinspector_renderer_contract_tests
ctest --test-dir build --output-on-failure -R '^renderer_contract$'
```

Expected: build fails because `RenderLoadProgress`, `RenderLoadStage`, and `setLoadProgressCallback()` do not exist.

- [ ] **Step 3: Add renderer progress interface**

Modify `src/renderer/RenderViewport.h`:

```cpp
enum class RenderLoadStage {
    Preparing,
    Uploading,
    FirstFrameReady,
};

struct RenderLoadProgress {
    RenderLoadStage stage = RenderLoadStage::Preparing;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
};
```

Add to `RenderViewport`:

```cpp
    using LoadProgressCallback =
        std::function<void(const RenderLoadProgress &)>;
```

Add pure virtual method:

```cpp
    virtual void setLoadProgressCallback(LoadProgressCallback callback) = 0;
```

- [ ] **Step 4: Update test fake viewports**

In `tests/main_window_tests.cpp`, add to `FakeViewport`:

```cpp
    void setLoadProgressCallback(LoadProgressCallback callback) override
    {
        loadProgressCallback_ = std::move(callback);
    }

    void emitLoadProgress(const pci::RenderLoadProgress &progress)
    {
        if (loadProgressCallback_) {
            loadProgressCallback_(progress);
        }
    }
```

Add private member:

```cpp
    LoadProgressCallback loadProgressCallback_;
```

In `tests/metal_point_picker_tests.cpp`, no fake viewport exists, so only the real `MetalRenderViewport` implementation is needed.

- [ ] **Step 5: Implement callback storage in Metal viewport**

Modify `src/renderer/metal/MetalRenderViewport.h`:

```cpp
    void setLoadProgressCallback(LoadProgressCallback callback) override;
```

Add private helpers:

```cpp
    void publishLoadProgress(RenderLoadProgress progress);
```

Add private members:

```cpp
    LoadProgressCallback loadProgressCallback_;
    std::uint64_t pointCloudGeneration_ = 0;
    bool pointCloudFrameReadyPending_ = false;
```

Modify `src/renderer/metal/MetalRenderViewport.cpp`:

```cpp
void MetalRenderViewport::setLoadProgressCallback(
    LoadProgressCallback callback)
{
    loadProgressCallback_ = std::move(callback);
}

void MetalRenderViewport::publishLoadProgress(RenderLoadProgress progress)
{
    if (loadProgressCallback_) {
        loadProgressCallback_(progress);
    }
}
```

- [ ] **Step 6: Publish renderer preparing, upload, and first-frame-ready progress**

In `MetalRenderViewport::setPointCloud()` after `pointResourcesDirty_ = true;`:

```cpp
    ++pointCloudGeneration_;
    pointCloudFrameReadyPending_ = true;
    publishLoadProgress({
        .stage = RenderLoadStage::Preparing,
        .completed = 0,
        .total = pointCount_,
    });
```

In `MetalRenderViewport::uploadPoints()`, after each `commandBuffer->resourceUpdate(updates);`:

```cpp
        if (pointCloud_) {
            publishLoadProgress({
                .stage = RenderLoadStage::Uploading,
                .completed = std::min(first + count, pointCount_),
                .total = pointCount_,
            });
        }
```

In `MetalRenderViewport::render()`, after `commandBuffer->endPass();` and before `++renderedFrames_;`:

```cpp
    if (pointCloudFrameReadyPending_) {
        pointCloudFrameReadyPending_ = false;
        publishLoadProgress({
            .stage = RenderLoadStage::FirstFrameReady,
            .completed = pointCount_,
            .total = pointCount_,
        });
    }
```

- [ ] **Step 7: Run renderer tests**

Run:

```bash
cmake --build build --target pcinspector_renderer_contract_tests pcinspector_metal_point_picker_tests
ctest --test-dir build --output-on-failure -R '^(renderer_contract|metal_point_picker)$'
```

Expected: `renderer_contract` and `metal_point_picker` pass. If `metal_point_picker` fails with `Cannot create window: no screens available`, rerun the same CTest command with WindowServer access.

- [ ] **Step 8: Commit renderer load progress callbacks**

```bash
git add src/renderer/RenderViewport.h src/renderer/metal/MetalRenderViewport.h src/renderer/metal/MetalRenderViewport.cpp tests/renderer_contract_tests.cpp tests/metal_point_picker_tests.cpp tests/main_window_tests.cpp
git commit -m "feat: report renderer loading readiness"
```

---

## Task 4: Map staged progress in MainWindow

**Files:**
- Modify: `src/app/MainWindow.h`
- Modify: `src/app/MainWindow.cpp`
- Modify: `tests/main_window_tests.cpp`

- [ ] **Step 1: Write failing MainWindow progress mapping tests**

Modify `ImmediateLoader` in `tests/main_window_tests.cpp` to emit staged progress:

```cpp
        if (request.progress) {
            request.progress({
                .stage = pci::PointCloudImportStage::Reading,
                .processed = 0,
                .total = 3,
            });
        }
```

Replace the final progress emission:

```cpp
        if (request.progress) {
            request.progress({
                .stage = pci::PointCloudImportStage::Reading,
                .processed = 3,
                .total = 3,
            });
            request.progress({
                .stage = pci::PointCloudImportStage::Optimizing,
                .processed = 3,
                .total = 3,
            });
        }
```

Modify `SlowLoader` progress:

```cpp
        if (request.progress) {
            request.progress({
                .stage = pci::PointCloudImportStage::Reading,
                .processed = 5,
                .total = 10,
            });
        }
```

Add test:

```cpp
void testReadingProgressMapsToFirstThirdOfProgressBar()
{
    auto loader = std::make_shared<SlowLoader>();
    auto controller = std::make_unique<pci::PointCloudLoadController>(loader);
    pci::MainWindow window(
        std::make_unique<FakeViewport>(), std::move(controller), 100);
    window.show();

    window.loadPointCloud("fixture.las");
    processUntil([&] { return loader->started.load(); });

    auto *bar = window.findChild<QProgressBar *>(
        QStringLiteral("loadingProgressBar"));
    auto *details = window.findChild<QLabel *>(
        QStringLiteral("loadingDetailsLabel"));
    CHECK(bar != nullptr);
    CHECK(details != nullptr);
    processUntil([&] { return bar->value() == 16 || bar->value() == 17; });
    CHECK(details->text().contains(QStringLiteral("Reading point cloud")));

    auto *cancel = window.findChild<QPushButton *>(
        QStringLiteral("loadingCancelButton"));
    CHECK(cancel != nullptr);
    cancel->click();
}
```

Add test:

```cpp
void testOverlayWaitsForRendererFirstFrameBeforeCompleting()
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto controller = std::make_unique<pci::PointCloudLoadController>(
        std::make_shared<ImmediateLoader>());
    pci::MainWindow window(
        std::move(viewport), std::move(controller), 100);
    window.show();

    window.loadPointCloud("fixture.las");

    auto *overlay =
        window.findChild<QWidget *>(QStringLiteral("loadingOverlay"));
    auto *bar = window.findChild<QProgressBar *>(
        QStringLiteral("loadingProgressBar"));
    CHECK(overlay != nullptr);
    CHECK(bar != nullptr);

    processUntil([&] {
        return viewportPointer->totalPointCount() == 3
               && !overlay->isHidden()
               && bar->value() < 100;
    });

    viewportPointer->emitLoadProgress({
        .stage = pci::RenderLoadStage::Uploading,
        .completed = 3,
        .total = 3,
    });
    CHECK(bar->value() == 98);

    viewportPointer->emitLoadProgress({
        .stage = pci::RenderLoadStage::FirstFrameReady,
        .completed = 3,
        .total = 3,
    });

    processUntil([&] { return overlay->isHidden(); });
    CHECK(window.statusBar()->currentMessage().contains("3 points"));
}
```

Call both tests from `main()`:

```cpp
        testReadingProgressMapsToFirstThirdOfProgressBar();
        testOverlayWaitsForRendererFirstFrameBeforeCompleting();
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build --target pcinspector_main_window_tests
ctest --test-dir build --output-on-failure -R '^main_window$'
```

Expected: build or test fails because `MainWindow` still expects raw progress and hides overlay immediately after `setPointCloud()`.

- [ ] **Step 3: Add MainWindow stage/timer members**

Modify `src/app/MainWindow.h` includes:

```cpp
#include "import/PointCloudImport.h"
#include "renderer/RenderViewport.h"
```

Remove the old `#include "renderer/RenderMetrics.h"` if `RenderViewport.h` now covers it.

Add private methods:

```cpp
    void showLoadProgress(
        PointCloudImportStage stage,
        std::uint64_t processed,
        std::uint64_t total);
    void showRenderLoadProgress(const RenderLoadProgress &progress);
    void beginEstimatedProgress(
        int startPercent,
        int endPercent,
        int estimateMilliseconds,
        const QString &details);
    void updateEstimatedProgress();
    void setLoadingProgress(int percentage, const QString &details);
    void completeLoadingDisplay();
```

Replace old `showLoadProgress(std::uint64_t, std::uint64_t)` declaration.

Add private members:

```cpp
    QTimer *loadingProgressTimer_ = nullptr;
    QElapsedTimer loadingStageTimer_;
    int loadingStageStartPercent_ = 0;
    int loadingStageEndPercent_ = 0;
    int loadingStageEstimateMilliseconds_ = 1;
    int currentLoadingPercent_ = 0;
    QString loadingStageDetails_;
    QString loadedPointCountText_;
```

Add these includes to `src/app/MainWindow.h`:

```cpp
#include <QElapsedTimer>
#include <QTimer>
```

- [ ] **Step 4: Wire viewport and load-controller progress callbacks**

Modify `src/app/MainWindow.cpp` includes:

```cpp
#include <algorithm>
#include <QElapsedTimer>
#include <QTimer>
```

After `viewport_->setFailureCallback(...)`:

```cpp
    viewport_->setLoadProgressCallback(
        [this](const RenderLoadProgress &progress) {
            showRenderLoadProgress(progress);
        });
```

Create the timer in the constructor before signal connections:

```cpp
    loadingProgressTimer_ = new QTimer(this);
    loadingProgressTimer_->setInterval(50);
    connect(loadingProgressTimer_, &QTimer::timeout,
            this, [this] { updateEstimatedProgress(); });
```

Update the controller progress connection:

```cpp
    connect(loadController_.get(),
            &PointCloudLoadController::progressChanged,
            this,
            [this](const PointCloudImportStage stage,
                   const quint64 processed,
                   const quint64 total) {
                showLoadProgress(stage, processed, total);
            });
```

- [ ] **Step 5: Implement mapped progress helpers**

Add near the top of `src/app/MainWindow.cpp`:

```cpp
namespace {

constexpr int readingEndPercent = 33;
constexpr int optimizingStartPercent = 33;
constexpr int optimizingEndPercent = 80;
constexpr int rendererStartPercent = 80;
constexpr int rendererEndPercent = 98;
constexpr int firstFrameReadyPercent = 100;

constexpr int optimizingEstimateMilliseconds = 4200;
constexpr int rendererEstimateMilliseconds = 1600;

int scaledPercent(const std::uint64_t processed,
                  const std::uint64_t total,
                  const int startPercent,
                  const int endPercent)
{
    if (total == 0) {
        return startPercent;
    }
    const auto span = static_cast<std::uint64_t>(endPercent - startPercent);
    const auto clamped = std::min(processed, total);
    return startPercent
        + static_cast<int>((clamped * span) / total);
}

} // namespace
```

Implement helpers:

```cpp
void MainWindow::setLoadingProgress(const int percentage,
                                    const QString &details)
{
    currentLoadingPercent_ = std::clamp(percentage, 0, 100);
    loadingOverlay_->setProgress(currentLoadingPercent_, details);
}

void MainWindow::beginEstimatedProgress(
    const int startPercent,
    const int endPercent,
    const int estimateMilliseconds,
    const QString &details)
{
    loadingStageStartPercent_ = std::max(currentLoadingPercent_, startPercent);
    loadingStageEndPercent_ = endPercent;
    loadingStageEstimateMilliseconds_ = std::max(1, estimateMilliseconds);
    loadingStageDetails_ = details;
    loadingStageTimer_.restart();
    setLoadingProgress(loadingStageStartPercent_, details);
    loadingProgressTimer_->start();
}

void MainWindow::updateEstimatedProgress()
{
    if (!loading_) {
        loadingProgressTimer_->stop();
        return;
    }
    const int cappedEnd = std::max(
        loadingStageStartPercent_,
        loadingStageEndPercent_ - 1);
    const double t = std::min(
        1.0,
        static_cast<double>(loadingStageTimer_.elapsed())
            / static_cast<double>(loadingStageEstimateMilliseconds_));
    const int percent = loadingStageStartPercent_
        + static_cast<int>(
            (cappedEnd - loadingStageStartPercent_) * t);
    if (percent > currentLoadingPercent_) {
        loadingOverlay_->setProgress(percent, loadingStageDetails_);
        currentLoadingPercent_ = percent;
    }
}
```

- [ ] **Step 6: Implement import and renderer progress mapping**

Replace old `MainWindow::showLoadProgress(...)` with:

```cpp
void MainWindow::showLoadProgress(const PointCloudImportStage stage,
                                  const std::uint64_t processed,
                                  const std::uint64_t total)
{
    if (!loading_) {
        return;
    }

    if (stage == PointCloudImportStage::Reading) {
        const int percent = scaledPercent(
            processed, total, 0, readingEndPercent);
        setLoadingProgress(
            percent,
            QStringLiteral("Reading point cloud… %1 / %2 source points")
                .arg(processed)
                .arg(total));
        statusBar()->showMessage(
            QStringLiteral("Loading %1: reading %2 / %3 source points")
                .arg(loadingSource_)
                .arg(processed)
                .arg(total));
        return;
    }

    beginEstimatedProgress(
        optimizingStartPercent,
        optimizingEndPercent,
        optimizingEstimateMilliseconds,
        QStringLiteral("Optimizing points…"));
    statusBar()->showMessage(
        QStringLiteral("Loading %1: optimizing points")
            .arg(loadingSource_));
}
```

Add:

```cpp
void MainWindow::showRenderLoadProgress(const RenderLoadProgress &progress)
{
    if (!loading_) {
        return;
    }

    if (progress.stage == RenderLoadStage::Preparing) {
        beginEstimatedProgress(
            rendererStartPercent,
            rendererEndPercent,
            rendererEstimateMilliseconds,
            QStringLiteral("Preparing renderer…"));
        return;
    }

    if (progress.stage == RenderLoadStage::Uploading) {
        const int percent = scaledPercent(
            progress.completed,
            progress.total,
            rendererStartPercent,
            rendererEndPercent);
        setLoadingProgress(
            percent,
            QStringLiteral("Uploading points to GPU… %1 / %2")
                .arg(progress.completed)
                .arg(progress.total));
        return;
    }

    setLoadingProgress(
        firstFrameReadyPercent,
        QStringLiteral("Displaying point cloud…"));
    completeLoadingDisplay();
}
```

Add:

```cpp
void MainWindow::completeLoadingDisplay()
{
    loadingProgressTimer_->stop();
    loadingOverlay_->hideLoading();
    loading_ = false;
    statusBar()->showMessage(loadedPointCountText_);
}
```

- [ ] **Step 7: Keep overlay visible until renderer first frame**

Modify `MainWindow::loadPointCloud()`:

```cpp
    currentLoadingPercent_ = 0;
    loadedPointCountText_.clear();
    loadingProgressTimer_->stop();
```

Call `setLoadingProgress(0, QStringLiteral("Reading point cloud…"));` after `showLoading(...)`.

Modify `finishLoading()`:

```cpp
        viewport_->setPointCloud(cloud);
        loadedPointCountText_ =
            QStringLiteral("Loaded %1 | %2 points")
                .arg(loadingSource_)
                .arg(cloud->points.size());
        statusBar()->showMessage(
            QStringLiteral("Loading %1: preparing renderer")
                .arg(loadingSource_));
```

Remove:

```cpp
        loadingOverlay_->hideLoading();
        loading_ = false;
```

Modify failure and cancellation paths to stop the timer:

```cpp
    if (loadingProgressTimer_) {
        loadingProgressTimer_->stop();
    }
```

- [ ] **Step 8: Run main-window tests**

Run:

```bash
cmake --build build --target pcinspector_main_window_tests
ctest --test-dir build --output-on-failure -R '^main_window$'
```

Expected: `main_window` passes.

- [ ] **Step 9: Commit MainWindow progress mapping**

```bash
git add src/app/MainWindow.h src/app/MainWindow.cpp tests/main_window_tests.cpp
git commit -m "feat: map loading progress to measured stages"
```

---

## Task 5: Update smoke/main tests and run full verification

**Files:**
- No source edits expected.

- [ ] **Step 1: Build all targets**

Run:

```bash
cmake --build build
```

Expected: build succeeds. A failure here means an earlier task missed a `RenderViewport` implementation; stop and fix that missed implementation before continuing.

- [ ] **Step 2: Run focused tests**

Run:

```bash
ctest --test-dir build --output-on-failure -R '^(load_controller|loading_overlay|main_window|renderer_contract)$'
```

Expected: all focused tests pass.

- [ ] **Step 3: Run full test suite**

Run:

```bash
ctest --test-dir build --output-on-failure
```

Expected: all tests pass. If Metal/windowed tests fail with `Cannot create window: no screens available`, rerun the same command with WindowServer access.

- [ ] **Step 4: Build application target**

Run:

```bash
cmake --build build --target pcinspector
```

Expected: app target builds successfully.

- [ ] **Step 5: Commit final compatibility fixes only if verification found a missed implementation**

If Step 1–4 found a missed `RenderViewport` implementation or another integration compile error, commit the corrected files:

```bash
git add main.cpp src/renderer/RenderViewport.h src/renderer/metal/MetalRenderViewport.h src/renderer/metal/MetalRenderViewport.cpp tests/main_window_tests.cpp tests/renderer_contract_tests.cpp tests/metal_point_picker_tests.cpp
git commit -m "fix: complete staged loading progress integration"
```

If no source fixes were required, do not create an empty commit.

---

## Manual verification

After automated tests pass, run the application with a large real point cloud and verify the observed behavior:

1. During PDAL reading, the progress bar advances from 0% to roughly 33%.
2. When PDAL source reading reaches completion, the overlay text changes to `Optimizing points…` and the bar continues moving through the 33–80% range.
3. After the worker returns, the overlay text changes to renderer preparation/upload wording and the bar advances through 80–98%.
4. The bar reaches 100% only when the new point cloud becomes visible.
5. The overlay hides immediately after that first visible frame.

If the 33/80/98 breakpoints are still visibly wrong on multiple real files, adjust only the constants in `MainWindow.cpp`; do not change the staged architecture.
