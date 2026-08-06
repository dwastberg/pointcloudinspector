#include "development/SyntheticScene.h"
#include "renderer/rhi/BackendPolicy.h"
#include "renderer/rhi/RenderViewportWidget_p.h"
#include "renderer/rhi/ShaderLoader.h"
#include "support/RenderViewportTestAccess.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>

#include <limits>
#include <memory>

namespace {

TEST_CASE("render viewport routes navigation input internally",
          "[ui][renderer-internal][input]")
{
    pci::RenderViewportWidget viewport(true);
    auto document = std::make_shared<pci::SceneDocument>();
    static_cast<void>(document->addLayer(pci::buildSyntheticScene(1'000)));
    viewport.setDocument(document->snapshot(), true);

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
