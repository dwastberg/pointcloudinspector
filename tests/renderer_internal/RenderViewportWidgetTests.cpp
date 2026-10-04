#include "support/RenderViewportTestAccess.h"
#include "support/SceneRuntimeFixture.h"
#include <pci/desktop/viewport/BackendPolicy.h>
#include <pci/desktop/viewport/RenderViewportWidget_p.h>
#include <pci/development/SyntheticScene.h>
#include <pci/document/SceneDocument.h>
#include <pci/rendering/rhi/ShaderLoader.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>

#include <limits>
#include <memory>
#include <stdexcept>

namespace {

TEST_CASE("render viewport requires paired document and runtime snapshots",
          "[ui][renderer-internal][runtime][architecture]")
{
    pci::RenderViewportWidget viewport(true);
    auto document = std::make_shared<pci::SceneDocument>();
    CHECK_THROWS_AS(
        viewport.setDocument(
            document->snapshot(),
            {},
            {.decodedPointBytes =
                 pci::HierarchyResidencyCoordinator::defaultByteBudget},
            pci::SessionGeneration{1},
            false),
        std::invalid_argument);
    CHECK_THROWS_AS(
        viewport.setDocument(document->snapshot(),
                             pci::test::runtimeSnapshotForTest(*document),
                             {},
                             pci::SessionGeneration{1},
                             false),
        std::invalid_argument);
}

TEST_CASE("render viewport routes navigation input internally",
          "[ui][renderer-internal][input]")
{
    pci::RenderViewportWidget viewport(true);
    auto document = std::make_shared<pci::SceneDocument>();
    pci::test::SceneRuntimeFixture runtime(document);
    static_cast<void>(runtime.addPointLayer(pci::buildSyntheticScene(1'000)));
    runtime.setDocument(viewport, true);

    const auto initialRevision =
        pci::testAccess(viewport).cameraForTesting().revision();
    QMouseEvent press(QEvent::MouseButtonPress,
                      QPointF(20.0, 20.0),
                      QPointF(20.0, 20.0),
                      Qt::LeftButton,
                      Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&viewport, &press);
    QMouseEvent move(QEvent::MouseMove,
                     QPointF(40.0, 20.0),
                     QPointF(40.0, 20.0),
                     Qt::NoButton,
                     Qt::LeftButton,
                     Qt::NoModifier);
    QApplication::sendEvent(&viewport, &move);
    CHECK(pci::testAccess(viewport).cameraForTesting().revision() >
          initialRevision);

    QKeyEvent forwardPress(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &forwardPress);
    CHECK(pci::testAccess(viewport).inputForTesting().movementDirection().z >
          0.0);
    QFocusEvent focusOut(QEvent::FocusOut);
    QApplication::sendEvent(&viewport, &focusOut);
    CHECK(
        pci::length(
            pci::testAccess(viewport).inputForTesting().movementDirection()) ==
        0.0);

    QKeyEvent frame(QEvent::KeyPress, Qt::Key_F, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &frame);
    CHECK(pci::testAccess(viewport).cameraForTesting().position() ==
          pci::Vec3d{0.0, -4.0, 0.0});
}

TEST_CASE("map view locks top-down orthographic GIS navigation",
          "[ui][renderer-internal][input][map]")
{
    pci::RenderViewportWidget viewport(true);
    viewport.resize(1000, 1000);
    auto document = std::make_shared<pci::SceneDocument>();
    pci::test::SceneRuntimeFixture runtime(document);
    static_cast<void>(runtime.addPointLayer(pci::buildSyntheticScene(1'000)));
    runtime.setDocument(viewport, true);

    viewport.setMapView(true);
    REQUIRE(viewport.isMapView());
    CHECK(viewport.isOrthographic());
    const pci::NavigationCamera &camera =
        pci::testAccess(viewport).cameraForTesting();
    CHECK(camera.forward() == pci::Vec3d{0.0, 0.0, -1.0});

    const pci::Vec3d pivotBeforeDrag = camera.pivot();
    QMouseEvent press(QEvent::MouseButtonPress,
                      QPointF(20.0, 20.0),
                      QPointF(20.0, 20.0),
                      Qt::LeftButton,
                      Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&viewport, &press);
    QMouseEvent move(QEvent::MouseMove,
                     QPointF(40.0, 30.0),
                     QPointF(40.0, 30.0),
                     Qt::NoButton,
                     Qt::LeftButton,
                     Qt::NoModifier);
    QApplication::sendEvent(&viewport, &move);
    CHECK(camera.forward() == pci::Vec3d{0.0, 0.0, -1.0});
    CHECK(camera.pivot().x < pivotBeforeDrag.x);
    CHECK(camera.pivot().y > pivotBeforeDrag.y);

    QMouseEvent release(QEvent::MouseButtonRelease,
                        QPointF(40.0, 30.0),
                        QPointF(40.0, 30.0),
                        Qt::LeftButton,
                        Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(&viewport, &release);

    const pci::Vec3d pivotBeforeKeyboard = camera.pivot();
    QKeyEvent forwardPress(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &forwardPress);
    pci::testAccess(viewport).advanceKeyboardNavigationForTesting(0.1);
    CHECK(camera.pivot().x == Catch::Approx(pivotBeforeKeyboard.x));
    CHECK(camera.pivot().y > pivotBeforeKeyboard.y);
    CHECK(camera.pivot().z == Catch::Approx(pivotBeforeKeyboard.z));

    viewport.setOrthographic(false);
    CHECK(viewport.isOrthographic());
    CHECK(camera.forward() == pci::Vec3d{0.0, 0.0, -1.0});
    viewport.setMapView(false);
    CHECK_FALSE(viewport.isMapView());
    CHECK_FALSE(viewport.isOrthographic());
}

TEST_CASE("map view zooms with held Q and Z keys",
          "[ui][renderer-internal][input][map]")
{
    pci::RenderViewportWidget viewport(true);
    viewport.setMapView(true);
    const auto access = pci::testAccess(viewport);
    const pci::NavigationCamera &camera = access.cameraForTesting();
    const pci::Vec3d initialPosition = camera.position();
    const pci::Vec3d initialPivot = camera.pivot();
    const double initialScale = camera.orthographicScale();

    QKeyEvent zoomIn(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &zoomIn);
    pci::testAccess(viewport).advanceKeyboardNavigationForTesting(0.1);
    const double zoomedScale = camera.orthographicScale();
    CHECK(zoomedScale < initialScale);

    QKeyEvent releaseZoomIn(QEvent::KeyRelease, Qt::Key_Q, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &releaseZoomIn);
    pci::testAccess(viewport).advanceKeyboardNavigationForTesting(0.1);
    CHECK(camera.orthographicScale() == zoomedScale);

    QKeyEvent zoomOut(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &zoomOut);
    pci::testAccess(viewport).advanceKeyboardNavigationForTesting(0.1);
    CHECK(camera.orthographicScale() == Catch::Approx(initialScale));
    pci::testAccess(viewport).advanceKeyboardNavigationForTesting(0.1);
    CHECK(camera.orthographicScale() > initialScale);

    SECTION("releasing Z stops zooming")
    {
        QKeyEvent releaseZoomOut(QEvent::KeyRelease, Qt::Key_Z, Qt::NoModifier);
        QApplication::sendEvent(&viewport, &releaseZoomOut);
    }
    SECTION("losing focus stops zooming")
    {
        QFocusEvent focusOut(QEvent::FocusOut);
        QApplication::sendEvent(&viewport, &focusOut);
    }
    const double stoppedScale = camera.orthographicScale();
    pci::testAccess(viewport).advanceKeyboardNavigationForTesting(0.1);
    CHECK(camera.orthographicScale() == stoppedScale);
    CHECK_FALSE(access.inputForTesting().hasMovement());
    CHECK(camera.position() == initialPosition);
    CHECK(camera.pivot() == initialPivot);
    CHECK(camera.forward() == pci::Vec3d{0.0, 0.0, -1.0});
}

TEST_CASE("map zoom keys preserve 3D navigation after switching modes",
          "[ui][renderer-internal][input][map]")
{
    pci::RenderViewportWidget viewport(true);
    viewport.setMapView(true);
    QKeyEvent zoomIn(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &zoomIn);
    viewport.setMapView(false);
    CHECK_FALSE(pci::testAccess(viewport).inputForTesting().hasMovement());

    const pci::NavigationCamera &camera =
        pci::testAccess(viewport).cameraForTesting();
    const pci::Vec3d initialPosition = camera.position();
    const double initialScale = camera.orthographicScale();
    QKeyEvent unbound(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &unbound);
    pci::testAccess(viewport).advanceKeyboardNavigationForTesting(0.1);
    CHECK(camera.position() == initialPosition);

    QApplication::sendEvent(&viewport, &zoomIn);
    pci::testAccess(viewport).advanceKeyboardNavigationForTesting(0.1);
    CHECK(camera.position().z < initialPosition.z);
    CHECK(camera.orthographicScale() == initialScale);
}

TEST_CASE("render viewport reports internal QRhi failures",
          "[ui][renderer-internal][failure]")
{
    pci::RenderViewportWidget viewport(true);
    QString rendererFailure;
    viewport.setFailureCallback([&rendererFailure](const QString &message) {
        rendererFailure = message;
    });
    REQUIRE(QMetaObject::invokeMethod(
        &viewport, "renderFailed", Qt::DirectConnection));
    CHECK(rendererFailure.contains(QStringLiteral("QRhiWidget")));

    QString shaderError;
    const QShader missingShader =
        pci::loadShaderResource(QStringLiteral(":/missing.qsb"), &shaderError);
    CHECK_FALSE(missingShader.isValid());
    CHECK(shaderError.contains(QStringLiteral("missing.qsb")));
}

TEST_CASE("render viewport applies an explicit backend before visibility",
          "[ui][renderer-internal][backend]")
{
    pci::RenderViewportWidget viewport(
        false,
        pci::UploadScheduler::defaultResidencyByteBudget,
        pci::GraphicsApi::OpenGL,
        true);
    CHECK(viewport.api() == QRhiWidget::Api::OpenGL);
    CHECK(viewport.isDebugLayerEnabled());
    CHECK(viewport.backendName() == QStringLiteral("OpenGL"));
}

TEST_CASE("render viewport clamps point size internally",
          "[ui][renderer-internal][point-size]")
{
    pci::RenderViewportWidget viewport(false);
    CHECK(viewport.pointSizePixels() == pci::defaultPointSizePixels);

    viewport.setPointSizePixels(pci::maximumPointSizePixels);
    CHECK(viewport.pointSizePixels() == pci::maximumPointSizePixels);

    viewport.setPointSizePixels(pci::maximumPointSizePixels + 1);
    CHECK(viewport.pointSizePixels() == pci::maximumPointSizePixels);

    viewport.setPointSizePixels(pci::minimumPointSizePixels - 1);
    CHECK(viewport.pointSizePixels() == pci::minimumPointSizePixels);
}

TEST_CASE("render viewport bounds appearance and depth settings",
          "[ui][renderer-internal][settings]")
{
    pci::RenderViewportWidget viewport(false);
    pci::ViewportSettings settings;
    settings.backgroundColor = {
        .red = -1.0F,
        .green = 2.0F,
        .blue = std::numeric_limits<float>::quiet_NaN(),
    };
    settings.depthEnhancement.radius = 100.0F;
    settings.depthEnhancement.strength = -10.0F;
    viewport.setViewportSettings(settings);

    const pci::ViewportSettings bounded = viewport.viewportSettings();
    CHECK(bounded.backgroundColor.red == 0.0F);
    CHECK(bounded.backgroundColor.green == 1.0F);
    CHECK(bounded.backgroundColor.blue == pci::defaultBackgroundBlue);
    CHECK(bounded.depthEnhancement.radius ==
          pci::maximumDepthEnhancementRadius);
    CHECK(bounded.depthEnhancement.strength ==
          pci::minimumDepthEnhancementStrength);

    viewport.setGpuByteBudget(std::uint64_t{256} * 1024 * 1024);
    CHECK(viewport.gpuByteBudget() == std::uint64_t{256} * 1024 * 1024);
}

} // namespace
