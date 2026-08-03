# Loading Progress Overlay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a clear centered loading overlay with progress and cancellation while point cloud files are imported.

**Architecture:** Add a focused `LoadingOverlay` Qt widget that owns only presentation state. Integrate it in `MainWindow` by wrapping the existing viewport widget in a stacked central container where the renderer remains visible under a dimmed overlay. Drive the overlay from existing `PointCloudLoadController` progress, loaded, failed, and cancelled signals.

**Tech Stack:** C++23, Qt 6 Widgets, Qt Concurrent-backed existing import controller, CMake/CTest.

---

## File structure

- Create `src/app/LoadingOverlay.h`: declares the widget API, stable test object names, and `cancelRequested()` signal.
- Create `src/app/LoadingOverlay.cpp`: builds the dimmed full-window overlay and centered progress panel.
- Create `tests/loading_overlay_tests.cpp`: isolated widget tests for initial state, determinate progress, indeterminate progress, and cancelling state.
- Modify `src/app/MainWindow.h`: add `LoadingOverlay` forward declaration/member and cancellation helper.
- Modify `src/app/MainWindow.cpp`: install stacked viewport/overlay central widget and wire load-controller signals to overlay state.
- Modify `tests/main_window_tests.cpp`: add integration tests for overlay visibility, progress, cancellation, success hide, and failure hide.
- Modify `CMakeLists.txt`: add overlay sources to app and tests, plus a standalone overlay test executable.

---

## Task 1: Standalone LoadingOverlay widget

**Files:**
- Create: `src/app/LoadingOverlay.h`
- Create: `src/app/LoadingOverlay.cpp`
- Create: `tests/loading_overlay_tests.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing standalone overlay tests**

Create `tests/loading_overlay_tests.cpp` with:

```cpp
#include "app/LoadingOverlay.h"

#include <QApplication>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(const bool condition, const char *expression, const int line)
{
    if (!condition) {
        throw std::runtime_error(
            "line " + std::to_string(line) + ": check failed: " + expression);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

template <typename T>
T *requiredChild(QObject &parent, const QString &objectName)
{
    T *child = parent.findChild<T *>(objectName);
    CHECK(child != nullptr);
    return child;
}

void testOverlayStartsHidden()
{
    pci::LoadingOverlay overlay;

    CHECK(overlay.objectName() == QStringLiteral("loadingOverlay"));
    CHECK(overlay.isHidden());
}

void testDeterminateProgressShowsFilenamePercentageAndPointCounts()
{
    pci::LoadingOverlay overlay;

    overlay.showLoading(QStringLiteral("/tmp/fixture.las"));
    overlay.updateProgress(25, 100);

    auto *title =
        requiredChild<QLabel>(overlay, QStringLiteral("loadingTitleLabel"));
    auto *details =
        requiredChild<QLabel>(overlay, QStringLiteral("loadingDetailsLabel"));
    auto *bar =
        requiredChild<QProgressBar>(overlay, QStringLiteral("loadingProgressBar"));
    auto *cancel =
        requiredChild<QPushButton>(overlay, QStringLiteral("loadingCancelButton"));

    CHECK(!overlay.isHidden());
    CHECK(title->text().contains(QStringLiteral("fixture.las")));
    CHECK(bar->minimum() == 0);
    CHECK(bar->maximum() == 100);
    CHECK(bar->value() == 25);
    CHECK(details->text().contains(QStringLiteral("25%")));
    CHECK(details->text().contains(QStringLiteral("25 / 100 points")));
    CHECK(cancel->isEnabled());
}

void testUnknownTotalUsesBusyProgress()
{
    pci::LoadingOverlay overlay;

    overlay.showLoading(QStringLiteral("unknown.laz"));
    overlay.updateProgress(0, 0);

    auto *details =
        requiredChild<QLabel>(overlay, QStringLiteral("loadingDetailsLabel"));
    auto *bar =
        requiredChild<QProgressBar>(overlay, QStringLiteral("loadingProgressBar"));

    CHECK(bar->minimum() == 0);
    CHECK(bar->maximum() == 0);
    CHECK(details->text().contains(QStringLiteral("Reading point cloud")));
}

void testCancelButtonEmitsSignalAndDisablesItself()
{
    pci::LoadingOverlay overlay;
    QSignalSpy cancelSpy(&overlay, &pci::LoadingOverlay::cancelRequested);

    overlay.showLoading(QStringLiteral("fixture.las"));
    auto *cancel =
        requiredChild<QPushButton>(overlay, QStringLiteral("loadingCancelButton"));
    cancel->click();

    auto *details =
        requiredChild<QLabel>(overlay, QStringLiteral("loadingDetailsLabel"));
    CHECK(cancelSpy.count() == 1);
    CHECK(!cancel->isEnabled());
    CHECK(details->text().contains(QStringLiteral("Cancelling")));
}

void testHideLoadingResetsState()
{
    pci::LoadingOverlay overlay;

    overlay.showLoading(QStringLiteral("fixture.las"));
    overlay.updateProgress(75, 100);
    overlay.setCancelling();
    overlay.hideLoading();
    overlay.showLoading(QStringLiteral("second.las"));

    auto *bar =
        requiredChild<QProgressBar>(overlay, QStringLiteral("loadingProgressBar"));
    auto *cancel =
        requiredChild<QPushButton>(overlay, QStringLiteral("loadingCancelButton"));

    CHECK(!overlay.isHidden());
    CHECK(bar->minimum() == 0);
    CHECK(bar->maximum() == 0);
    CHECK(cancel->isEnabled());
}

} // namespace

int main(int argc, char **argv)
{
    QApplication application(argc, argv);

    try {
        testOverlayStartsHidden();
        testDeterminateProgressShowsFilenamePercentageAndPointCounts();
        testUnknownTotalUsesBusyProgress();
        testCancelButtonEmitsSignalAndDisablesItself();
        testHideLoadingResetsState();
        std::cout << "loading overlay tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

- [ ] **Step 2: Add the failing overlay test target**

Modify the `if(APPLE)` test section in `CMakeLists.txt` before `pcinspector_main_window_tests`:

```cmake
        add_executable(pcinspector_loading_overlay_tests
                tests/loading_overlay_tests.cpp
                src/app/LoadingOverlay.cpp
                src/app/LoadingOverlay.h)
        target_include_directories(pcinspector_loading_overlay_tests PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(pcinspector_loading_overlay_tests PRIVATE
                Qt6::Core
                Qt6::Test
                Qt6::Widgets)
        add_test(NAME loading_overlay
                COMMAND pcinspector_loading_overlay_tests)
        set_tests_properties(loading_overlay PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
```

- [ ] **Step 3: Run test to verify it fails**

Run:

```bash
cmake --build build --target pcinspector_loading_overlay_tests
```

Expected: build fails because `app/LoadingOverlay.h` does not exist yet.

- [ ] **Step 4: Implement `LoadingOverlay` header**

Create `src/app/LoadingOverlay.h`:

```cpp
#pragma once

#include <QWidget>

#include <cstdint>

class QLabel;
class QProgressBar;
class QPushButton;

namespace pci {

class LoadingOverlay final : public QWidget {
    Q_OBJECT

public:
    explicit LoadingOverlay(QWidget *parent = nullptr);

    void showLoading(const QString &sourcePath);
    void updateProgress(std::uint64_t processed, std::uint64_t total);
    void setCancelling();
    void hideLoading();

signals:
    void cancelRequested();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void updatePanelWidth();

    QLabel *titleLabel_ = nullptr;
    QLabel *detailsLabel_ = nullptr;
    QProgressBar *progressBar_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QString sourceName_;
    bool cancelling_ = false;
};

} // namespace pci
```

- [ ] **Step 5: Implement `LoadingOverlay` source**

Create `src/app/LoadingOverlay.cpp`:

```cpp
#include "app/LoadingOverlay.h"

#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>

#include <algorithm>

namespace pci {

LoadingOverlay::LoadingOverlay(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("loadingOverlay"));
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(false);
    setStyleSheet(QStringLiteral(
        "QWidget#loadingOverlay {"
        "  background-color: rgba(0, 0, 0, 120);"
        "}"
        "QFrame#loadingPanel {"
        "  background-color: rgba(32, 32, 36, 235);"
        "  border: 1px solid rgba(255, 255, 255, 70);"
        "  border-radius: 10px;"
        "}"
        "QLabel { color: white; }"
        "QProgressBar {"
        "  min-height: 18px;"
        "  color: white;"
        "  text-align: center;"
        "}"
        "QProgressBar::chunk { background-color: #4f9cff; }"));

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(24, 24, 24, 24);
    rootLayout->addStretch(1);

    auto *centerRow = new QHBoxLayout;
    centerRow->addStretch(1);

    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("loadingPanel"));
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(24, 20, 24, 20);
    panelLayout->setSpacing(12);

    titleLabel_ = new QLabel(panel);
    titleLabel_->setObjectName(QStringLiteral("loadingTitleLabel"));
    titleLabel_->setAlignment(Qt::AlignCenter);
    titleLabel_->setWordWrap(true);
    panelLayout->addWidget(titleLabel_);

    progressBar_ = new QProgressBar(panel);
    progressBar_->setObjectName(QStringLiteral("loadingProgressBar"));
    progressBar_->setTextVisible(true);
    panelLayout->addWidget(progressBar_);

    detailsLabel_ = new QLabel(panel);
    detailsLabel_->setObjectName(QStringLiteral("loadingDetailsLabel"));
    detailsLabel_->setAlignment(Qt::AlignCenter);
    panelLayout->addWidget(detailsLabel_);

    cancelButton_ = new QPushButton(QStringLiteral("Cancel"), panel);
    cancelButton_->setObjectName(QStringLiteral("loadingCancelButton"));
    panelLayout->addWidget(cancelButton_, 0, Qt::AlignCenter);

    connect(cancelButton_, &QPushButton::clicked, this, [this] {
        setCancelling();
        emit cancelRequested();
    });

    centerRow->addWidget(panel);
    centerRow->addStretch(1);
    rootLayout->addLayout(centerRow);
    rootLayout->addStretch(1);

    hideLoading();
}

void LoadingOverlay::showLoading(const QString &sourcePath)
{
    const QFileInfo info(sourcePath);
    sourceName_ = info.fileName().isEmpty() ? sourcePath : info.fileName();
    cancelling_ = false;
    cancelButton_->setEnabled(true);
    titleLabel_->setText(QStringLiteral("Loading %1…").arg(sourceName_));
    progressBar_->setRange(0, 0);
    progressBar_->setValue(0);
    detailsLabel_->setText(QStringLiteral("Reading point cloud…"));
    updatePanelWidth();
    show();
    raise();
}

void LoadingOverlay::updateProgress(const std::uint64_t processed,
                                    const std::uint64_t total)
{
    if (total == 0) {
        progressBar_->setRange(0, 0);
        detailsLabel_->setText(cancelling_
                                   ? QStringLiteral("Cancelling…")
                                   : QStringLiteral("Reading point cloud…"));
        return;
    }

    const auto percentage =
        static_cast<int>(std::min<std::uint64_t>(100, (processed * 100) / total));
    progressBar_->setRange(0, 100);
    progressBar_->setValue(percentage);
    detailsLabel_->setText(
        cancelling_
            ? QStringLiteral("Cancelling… %1% | %2 / %3 points")
                  .arg(percentage)
                  .arg(processed)
                  .arg(total)
            : QStringLiteral("%1% | %2 / %3 points")
                  .arg(percentage)
                  .arg(processed)
                  .arg(total));
}

void LoadingOverlay::setCancelling()
{
    cancelling_ = true;
    cancelButton_->setEnabled(false);
    detailsLabel_->setText(QStringLiteral("Cancelling…"));
}

void LoadingOverlay::hideLoading()
{
    cancelling_ = false;
    sourceName_.clear();
    if (cancelButton_) {
        cancelButton_->setEnabled(true);
    }
    hide();
}

void LoadingOverlay::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updatePanelWidth();
}

void LoadingOverlay::updatePanelWidth()
{
    if (auto *panel = findChild<QFrame *>(QStringLiteral("loadingPanel"))) {
        const int targetWidth = std::clamp(width() - 96, 320, 560);
        panel->setFixedWidth(targetWidth);
    }
}

} // namespace pci
```

- [ ] **Step 6: Run standalone overlay test**

Run:

```bash
cmake --build build --target pcinspector_loading_overlay_tests
ctest --test-dir build --output-on-failure -R '^loading_overlay$'
```

Expected: `loading_overlay` passes.

- [ ] **Step 7: Commit standalone overlay widget**

```bash
git add CMakeLists.txt src/app/LoadingOverlay.h src/app/LoadingOverlay.cpp tests/loading_overlay_tests.cpp
git commit -m "feat: add loading progress overlay widget"
```

---

## Task 2: Integrate overlay with MainWindow loading flow

**Files:**
- Modify: `src/app/MainWindow.h`
- Modify: `src/app/MainWindow.cpp`
- Modify: `tests/main_window_tests.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing MainWindow integration tests**

Modify `tests/main_window_tests.cpp` to include:

```cpp
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
```

Add these helper loader classes next to `ImmediateLoader`:

```cpp
class SlowLoader final : public pci::PointCloudLoader {
public:
    mutable std::atomic<bool> started = false;

    pci::LoadedPointCloudPtr
    load(const pci::PointCloudImportRequest &request) const override
    {
        started = true;
        if (request.progress) {
            request.progress(1, 10);
        }
        while (!request.stopToken.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw pci::PointCloudImportCancelled();
    }
};

class FailingLoader final : public pci::PointCloudLoader {
public:
    pci::LoadedPointCloudPtr
    load(const pci::PointCloudImportRequest &) const override
    {
        throw pci::PointCloudImportError("fixture failure");
    }
};
```

Add required includes:

```cpp
#include <atomic>
#include <chrono>
#include <thread>
```

Add this helper:

```cpp
template <typename Predicate>
void processUntil(Predicate predicate)
{
    QElapsedTimer timeout;
    timeout.start();
    while (!predicate() && timeout.elapsed() < 2000) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(predicate());
}
```

Add tests:

```cpp
void testLoadingOverlayShowsProgressAndCancels()
{
    auto loader = std::make_shared<SlowLoader>();
    auto controller = std::make_unique<pci::PointCloudLoadController>(loader);
    pci::MainWindow window(
        std::make_unique<FakeViewport>(), std::move(controller), 100);
    window.show();

    window.loadPointCloud("fixture.las");
    processUntil([&] { return loader->started.load(); });
    processUntil([&] {
        auto *details = window.findChild<QLabel *>(
            QStringLiteral("loadingDetailsLabel"));
        return details != nullptr
               && details->text().contains(QStringLiteral("1 / 10 points"));
    });

    auto *overlay =
        window.findChild<QWidget *>(QStringLiteral("loadingOverlay"));
    auto *bar = window.findChild<QProgressBar *>(
        QStringLiteral("loadingProgressBar"));
    auto *cancel = window.findChild<QPushButton *>(
        QStringLiteral("loadingCancelButton"));

    CHECK(overlay != nullptr);
    CHECK(!overlay->isHidden());
    CHECK(bar != nullptr);
    CHECK(bar->value() == 10);
    CHECK(cancel != nullptr);
    CHECK(cancel->isEnabled());

    cancel->click();

    CHECK(!cancel->isEnabled());
    processUntil([&] { return overlay->isHidden(); });
    CHECK(window.statusBar()->currentMessage().contains("cancelled"));
}

void testLoadingOverlayHidesAfterSuccessfulLoad()
{
    auto controller = std::make_unique<pci::PointCloudLoadController>(
        std::make_shared<ImmediateLoader>());
    pci::MainWindow window(
        std::make_unique<FakeViewport>(), std::move(controller), 100);
    window.show();

    window.loadPointCloud("fixture.las");

    auto *overlay =
        window.findChild<QWidget *>(QStringLiteral("loadingOverlay"));
    CHECK(overlay != nullptr);
    processUntil([&] {
        return overlay->isHidden()
               && window.statusBar()->currentMessage().contains("3 points");
    });
}

void testLoadingOverlayHidesAfterFailedLoad()
{
    auto controller = std::make_unique<pci::PointCloudLoadController>(
        std::make_shared<FailingLoader>());
    pci::MainWindow window(
        std::make_unique<FakeViewport>(), std::move(controller), 100);
    window.show();

    window.loadPointCloud("broken.las");

    auto *overlay =
        window.findChild<QWidget *>(QStringLiteral("loadingOverlay"));
    CHECK(overlay != nullptr);
    processUntil([&] {
        return overlay->isHidden()
               && window.statusBar()->currentMessage().contains("Loading failed");
    });
}
```

Call the new tests from `main()`:

```cpp
        testLoadingOverlayShowsProgressAndCancels();
        testLoadingOverlayHidesAfterSuccessfulLoad();
        testLoadingOverlayHidesAfterFailedLoad();
```

- [ ] **Step 2: Add `LoadingOverlay` to the main app and main-window test targets**

Modify the `pcinspector` executable sources:

```cmake
            src/app/MainWindow.cpp
            src/app/MainWindow.h
            src/app/LoadingOverlay.cpp
            src/app/LoadingOverlay.h
```

Modify `pcinspector_main_window_tests` sources:

```cmake
                tests/main_window_tests.cpp
                src/app/MainWindow.cpp
                src/app/MainWindow.h
                src/app/LoadingOverlay.cpp
                src/app/LoadingOverlay.h)
```

- [ ] **Step 3: Run test to verify it fails**

Run:

```bash
cmake --build build --target pcinspector_main_window_tests
ctest --test-dir build --output-on-failure -R '^main_window$'
```

Expected: test fails because `MainWindow` does not yet create or wire `loadingOverlay`.

- [ ] **Step 4: Implement MainWindow header changes**

Modify `src/app/MainWindow.h`:

```cpp
class LoadingOverlay;
```

Add private method:

```cpp
    void requestLoadCancellation();
```

Add private member:

```cpp
    LoadingOverlay *loadingOverlay_ = nullptr;
```

- [ ] **Step 5: Implement MainWindow source integration**

Modify `src/app/MainWindow.cpp` includes:

```cpp
#include "app/LoadingOverlay.h"
```

Add Qt includes:

```cpp
#include <QStackedLayout>
#include <QWidget>
```

Replace:

```cpp
    setCentralWidget(viewport_->widget());
```

with:

```cpp
    auto *centralContainer = new QWidget(this);
    auto *centralStack = new QStackedLayout(centralContainer);
    centralStack->setContentsMargins(0, 0, 0, 0);
    centralStack->setStackingMode(QStackedLayout::StackAll);
    centralStack->addWidget(viewport_->widget());

    loadingOverlay_ = new LoadingOverlay(centralContainer);
    centralStack->addWidget(loadingOverlay_);
    loadingOverlay_->hideLoading();
    setCentralWidget(centralContainer);
```

Replace both direct cancellation connections with `requestLoadCancellation()`:

```cpp
    connect(cancelAction, &QAction::triggered,
            this, [this] { requestLoadCancellation(); });
    connect(loadingOverlay_, &LoadingOverlay::cancelRequested,
            this, [this] { requestLoadCancellation(); });
```

In `loadPointCloud`, after setting `loadingSource_`, show the overlay:

```cpp
    loadingOverlay_->showLoading(QString::fromStdString(sourcePath.string()));
```

In `showLoadProgress`, update the overlay before or after the status bar:

```cpp
    loadingOverlay_->updateProgress(processed, total);
```

In `finishLoading`, hide the overlay after successful `viewport_->setPointCloud(cloud)` and before setting `loading_ = false`:

```cpp
        loadingOverlay_->hideLoading();
```

In `showLoadFailure`, hide the overlay before updating the status bar:

```cpp
    if (loadingOverlay_) {
        loadingOverlay_->hideLoading();
    }
```

In the `cancelled` signal lambda, hide the overlay:

```cpp
                if (loadingOverlay_) {
                    loadingOverlay_->hideLoading();
                }
```

Add the helper:

```cpp
void MainWindow::requestLoadCancellation()
{
    if (!loading_) {
        return;
    }
    if (loadingOverlay_) {
        loadingOverlay_->setCancelling();
    }
    loadController_->cancel();
}
```

- [ ] **Step 6: Run main-window integration tests**

Run:

```bash
cmake --build build --target pcinspector_main_window_tests
ctest --test-dir build --output-on-failure -R '^main_window$'
```

Expected: `main_window` passes.

- [ ] **Step 7: Commit MainWindow integration**

```bash
git add CMakeLists.txt src/app/MainWindow.h src/app/MainWindow.cpp tests/main_window_tests.cpp
git commit -m "feat: show loading overlay during point cloud imports"
```

---

## Task 3: Full verification

**Files:**
- No source edits expected.

- [ ] **Step 1: Run focused loading/UI tests**

Run:

```bash
ctest --test-dir build --output-on-failure -R '^(loading_overlay|main_window|load_controller)$'
```

Expected: all three tests pass.

- [ ] **Step 2: Run full test suite**

Run:

```bash
ctest --test-dir build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 3: Build app target**

Run:

```bash
cmake --build build --target pcinspector
```

Expected: `pcinspector` builds successfully.

- [ ] **Step 4: Commit verification-only fixes if needed**

If verification exposes small issues, fix them with TDD where possible, rerun the failing command and then the focused tests, and commit:

```bash
git add CMakeLists.txt src/app/LoadingOverlay.h src/app/LoadingOverlay.cpp src/app/MainWindow.h src/app/MainWindow.cpp tests/loading_overlay_tests.cpp tests/main_window_tests.cpp
git commit -m "fix: stabilize loading overlay verification"
```

If no source changes are needed, do not create an empty commit.
