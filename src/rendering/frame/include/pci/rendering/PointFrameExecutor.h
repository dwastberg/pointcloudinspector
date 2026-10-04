#pragma once

#include <pci/rendering/planning/PointFrameCoordinator.h>
#include <pci/runtime/scene/SceneRuntime.h>

#include <cstdint>

namespace pci {

struct PointFrameExecutionContext {
    SessionGeneration sessionGeneration;
    DocumentGeneration documentGeneration;
    std::uint64_t documentRevision = 0;
    SceneRuntimeSnapshotPtr runtime;
};

enum class PointFrameExecutionOutcome : std::uint8_t {
    Applied,
    Stale,
    DuplicateOrOutOfOrder,
};

// Applies a completed point plan's runtime effects after validating every
// captured identity. Validation is all-or-nothing: a stale binding/content
// target cannot leave another layer partially reconciled.
class PointFrameExecutor final {
public:
    [[nodiscard]] static bool valid(const PointFrameResult &frame,
                                    const PointFrameExecutionContext &context);
    [[nodiscard]] PointFrameExecutionOutcome
    execute(const PointFrameResult &frame,
            const PointFrameExecutionContext &context);

private:
    [[nodiscard]] static bool
    validTarget(const PointFrameRuntimeTarget &target,
                const SceneRuntimeSnapshotPtr &runtime);

    std::size_t requestCursor_ = 0;
    std::uint64_t lastExecutionSerial_ = 0;
};

} // namespace pci
