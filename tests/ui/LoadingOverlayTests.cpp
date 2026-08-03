#include "app/LoadingOverlay.h"

#include <catch2/catch_test_macros.hpp>

#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

namespace {

template <typename T>
T *requiredChild(QObject &parent, const QString &objectName)
{
    T *child = parent.findChild<T *>(objectName);
    REQUIRE(child != nullptr);
    return child;
}

TEST_CASE("loading overlay starts hidden", "[ui][loading]")
{
    pci::LoadingOverlay overlay;

    CHECK(overlay.objectName() == QStringLiteral("loadingOverlay"));
    CHECK(overlay.isHidden());
}

TEST_CASE("loading overlay displays determinate progress", "[ui][loading]")
{
    pci::LoadingOverlay overlay;
    overlay.showLoading(QStringLiteral("/tmp/fixture.las"));
    overlay.updateProgress(25, 100);

    auto *title =
        requiredChild<QLabel>(overlay, QStringLiteral("loadingTitleLabel"));
    auto *details =
        requiredChild<QLabel>(overlay, QStringLiteral("loadingDetailsLabel"));
    auto *bar = requiredChild<QProgressBar>(
        overlay, QStringLiteral("loadingProgressBar"));
    auto *cancel = requiredChild<QPushButton>(
        overlay, QStringLiteral("loadingCancelButton"));

    CHECK_FALSE(overlay.isHidden());
    CHECK(title->text().contains(QStringLiteral("fixture.las")));
    CHECK(bar->minimum() == 0);
    CHECK(bar->maximum() == 100);
    CHECK(bar->value() == 25);
    CHECK(details->text().contains(QStringLiteral("25%")));
    CHECK(details->text().contains(QStringLiteral("25 / 100 points")));
    CHECK(cancel->isEnabled());
}

TEST_CASE("loading overlay supports unknown totals and direct progress",
          "[ui][loading]")
{
    pci::LoadingOverlay overlay;
    overlay.showLoading(QStringLiteral("unknown.laz"));
    overlay.updateProgress(0, 0);

    auto *details =
        requiredChild<QLabel>(overlay, QStringLiteral("loadingDetailsLabel"));
    auto *bar = requiredChild<QProgressBar>(
        overlay, QStringLiteral("loadingProgressBar"));
    CHECK(bar->minimum() == 0);
    CHECK(bar->maximum() == 0);
    CHECK(details->text().contains(QStringLiteral("Reading point cloud")));

    overlay.setProgress(33, QStringLiteral("Optimizing points"));
    CHECK(bar->minimum() == 0);
    CHECK(bar->maximum() == 100);
    CHECK(bar->value() == 33);
    CHECK(details->text() == QStringLiteral("Optimizing points"));
}

TEST_CASE("loading overlay cancellation emits a signal and resets",
          "[ui][loading]")
{
    pci::LoadingOverlay overlay;
    QSignalSpy cancelSpy(&overlay, &pci::LoadingOverlay::cancelRequested);
    REQUIRE(cancelSpy.isValid());

    overlay.showLoading(QStringLiteral("fixture.las"));
    auto *cancel = requiredChild<QPushButton>(
        overlay, QStringLiteral("loadingCancelButton"));
    cancel->click();

    auto *details =
        requiredChild<QLabel>(overlay, QStringLiteral("loadingDetailsLabel"));
    CHECK(cancelSpy.count() == 1);
    CHECK_FALSE(cancel->isEnabled());
    CHECK(details->text().contains(QStringLiteral("Cancelling")));

    overlay.hideLoading();
    overlay.showLoading(QStringLiteral("second.las"));
    auto *bar = requiredChild<QProgressBar>(
        overlay, QStringLiteral("loadingProgressBar"));
    CHECK_FALSE(overlay.isHidden());
    CHECK(bar->minimum() == 0);
    CHECK(bar->maximum() == 100);
    CHECK(bar->value() == 0);
    CHECK(cancel->isEnabled());
}

TEST_CASE("loading overlay visibly reaches completion before closing",
          "[ui][loading][progress]")
{
    pci::LoadingOverlay overlay;
    overlay.showLoading(QStringLiteral("fixture.las"));
    auto *bar = requiredChild<QProgressBar>(
        overlay, QStringLiteral("loadingProgressBar"));
    auto *details =
        requiredChild<QLabel>(overlay, QStringLiteral("loadingDetailsLabel"));
    CHECK(bar->value() == 0);

    overlay.setProgress(84, QStringLiteral("Preparing full detail"));
    overlay.showComplete(QStringLiteral("Ready"));
    CHECK_FALSE(overlay.isHidden());
    CHECK(bar->value() == 100);
    CHECK(details->text() == QStringLiteral("Ready"));
    REQUIRE(QTest::qWaitFor(
        [&] {
            return overlay.isHidden();
        },
        1000));
}

} // namespace
