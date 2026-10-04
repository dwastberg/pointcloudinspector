#pragma once

#include <pci/desktop/viewport/RenderViewport.h>
#include <pci/runtime/point/PointDatasetRuntime.h>

#include <pci/document/SceneDocument.h>
#include <pci/runtime/scene/SceneRuntime.h>

#include <optional>
#include <stdexcept>
#include <utility>

namespace pci::test {

// Renderer tests must provide live sources explicitly; the document contains
// only metadata and cannot be used to reconstruct runtime bindings.
inline SceneRuntimeSnapshotPtr
runtimeSnapshotForTest(const SceneDocument &document)
{
    if (document.hasPointCloudLayers() || document.rasterLayerCount() != 0) {
        throw std::logic_error("source-bearing renderer tests require an "
                               "explicit runtime fixture");
    }
    SceneRuntime runtime;
    return runtime.snapshot();
}

class SceneRuntimeFixture final {
public:
    explicit SceneRuntimeFixture(
        SceneDocumentPtr document,
        const std::uint64_t decodedByteBudget =
            HierarchyResidencyCoordinator::defaultByteBudget,
        const std::size_t maximumConcurrentDecodes =
            HierarchyResidencyCoordinator::defaultMaximumConcurrentDecodes)
        : document_(std::move(document))
        , runtime_(decodedByteBudget, maximumConcurrentDecodes)
    {
        if (!document_) {
            throw std::invalid_argument("test document must not be null");
        }
        if (document_->rasterLayerCount() != 0) {
            throw std::invalid_argument(
                "test runtime must attach raster layers itself");
        }
        if (document_->hasPointCloudLayers()) {
            throw std::invalid_argument(
                "test runtime must attach point layers itself");
        }
    }

    [[nodiscard]] PointCloudLayerId
    addPointLayer(const PointDatasetRuntimePtr &scene,
                  const bool initiallyVisible = true)
    {
        if (!scene) {
            throw std::invalid_argument(
                "test point layers require an attached scene");
        }
        const BindingGeneration binding =
            nextGeneration(document_->lastBindingGeneration());
        if (!runtime_.attachPoint({.descriptor = scene->descriptor(),
                                   .runtime = scene,
                                   .generation = binding,
                                   .active = initiallyVisible})) {
            throw std::logic_error("test point runtime attachment failed");
        }
        try {
            const PointCloudLayerId id =
                document_->addLayer(scene->datasetView(), binding);
            if (!initiallyVisible && !document_->setLayerVisible(id, false)) {
                throw std::logic_error(
                    "test point layer visibility update failed");
            }
            return id;
        } catch (...) {
            static_cast<void>(runtime_.detach(binding));
            throw;
        }
    }

    [[nodiscard]] SceneLayerId
    addRasterLayer(const RasterLayerDataPtr &data,
                   const bool initiallyVisible = true)
    {
        if (!data || !data->source) {
            throw std::invalid_argument(
                "test raster layers require an attached source");
        }
        const BindingGeneration binding =
            nextGeneration(document_->lastBindingGeneration());
        if (!runtime_.attachRaster({.descriptor = data->descriptor(),
                                    .source = data->source,
                                    .generation = binding})) {
            throw std::logic_error("test raster runtime attachment failed");
        }
        try {
            return document_->addRasterLayer(
                data->descriptor(), initiallyVisible, binding);
        } catch (...) {
            static_cast<void>(runtime_.detach(binding));
            throw;
        }
    }

    [[nodiscard]] bool setPointLayerVisible(const PointCloudLayerId id,
                                            const bool visible)
    {
        const std::optional<PointCloudLayer> layer = document_->layer(id);
        if (!layer) {
            return false;
        }
        if (layer->visible == visible) {
            return true;
        }
        if (!runtime_.setPointActive(layer->descriptor.sourceId,
                                     layer->bindingGeneration,
                                     visible)) {
            return false;
        }
        if (document_->setLayerVisible(id, visible)) {
            return true;
        }
        static_cast<void>(runtime_.setPointActive(layer->descriptor.sourceId,
                                                  layer->bindingGeneration,
                                                  layer->visible));
        return false;
    }

    [[nodiscard]] SceneRuntimeSnapshotPtr runtimeSnapshot() const
    {
        return runtime_.snapshot();
    }

    [[nodiscard]] const SceneRuntime &runtime() const noexcept
    {
        return runtime_;
    }

    [[nodiscard]] const SceneDocumentPtr &document() const noexcept
    {
        return document_;
    }

    void setDocument(
        RenderViewport &viewport,
        const bool frameVisibleLayers,
        const SessionGeneration sessionGeneration = SessionGeneration{1}) const
    {
        viewport.setDocument(
            document_->snapshot(),
            runtimeSnapshot(),
            {.decodedPointBytes = runtime_.decodedByteBudget()},
            sessionGeneration,
            frameVisibleLayers);
    }

    void updateDocument(
        RenderViewport &viewport,
        const SessionGeneration sessionGeneration = SessionGeneration{1}) const
    {
        viewport.updateDocument(
            document_->snapshot(),
            runtimeSnapshot(),
            {.decodedPointBytes = runtime_.decodedByteBudget()},
            sessionGeneration);
    }

private:
    SceneDocumentPtr document_;
    SceneRuntime runtime_;
};

inline void setTestDocument(
    RenderViewport &viewport,
    const SceneDocumentPtr &document,
    const bool frameVisibleLayers,
    const SessionGeneration sessionGeneration = SessionGeneration{1})
{
    SceneRuntimeSnapshotPtr runtime = runtimeSnapshotForTest(*document);
    viewport.setDocument(
        document->snapshot(),
        std::move(runtime),
        {.decodedPointBytes = HierarchyResidencyCoordinator::defaultByteBudget},
        sessionGeneration,
        frameVisibleLayers);
}

inline void setTestDocument(
    RenderViewport &viewport,
    const SceneRuntimeFixture &fixture,
    const bool frameVisibleLayers,
    const SessionGeneration sessionGeneration = SessionGeneration{1})
{
    fixture.setDocument(viewport, frameVisibleLayers, sessionGeneration);
}

inline void updateTestDocument(
    RenderViewport &viewport,
    const SceneDocumentPtr &document,
    const SessionGeneration sessionGeneration = SessionGeneration{1})
{
    SceneRuntimeSnapshotPtr runtime = runtimeSnapshotForTest(*document);
    viewport.updateDocument(
        document->snapshot(),
        std::move(runtime),
        {.decodedPointBytes = HierarchyResidencyCoordinator::defaultByteBudget},
        sessionGeneration);
}

} // namespace pci::test
