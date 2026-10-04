#pragma once
#include <functional>
#include <pci/desktop/session/DocumentUpdate.h>
#include <pci/document/SceneDocument.h>
#include <pci/runtime/point/PointDatasetRuntime.h>
#include <pci/runtime/scene/SceneRuntime.h>
namespace pci {
struct ScenePublicationContext {
    SceneDocumentPtr &document;
    SceneRuntime &runtime;
    SessionGeneration session;
    std::uint64_t decodedBytes;
    const std::function<void()> &beforeCommit;
    std::function<void(DocumentUpdate)> notify;
};
// Owner-thread transaction boundary shared by import and color installation.
class ScenePublicationCoordinator final {
public:
    explicit ScenePublicationCoordinator(ScenePublicationContext context)
        : context_(std::move(context))
    {
    }
    void commitPoints(PointCloudLayerId layer,
                      const PointDatasetRuntimePtr &runtime,
                      std::unique_ptr<PointDatasetPublication> publication);
    bool commitColors(SceneDocumentPtr candidate,
                      const PointDatasetRuntimePtr &runtime,
                      PointCloudLayerId layer,
                      std::unique_ptr<PointColorPublication> publication);
    void install(SceneDocumentPtr document,
                 SceneRuntime runtime,
                 DocumentUpdate update);

private:
    ScenePublicationContext context_;
};
} // namespace pci
