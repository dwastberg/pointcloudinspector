#pragma once

#include <pci/rendering/FrameExecutor.h>

namespace pci {

// GPU readback delivery has a separate lifetime from its storage. This guard
// checks content identity before any pick kind may update navigation or UI.
class FramePickGuard final {
public:
    [[nodiscard]] static FramePickGuard capture(const FrameContext &context);
    [[nodiscard]] bool valid(const FrameContext &context) const;

private:
    SessionGeneration session_;
    DocumentGeneration document_;
    std::uint64_t revision_ = 0;
    struct PointTarget {
        PointFrameRuntimeTarget runtime;
        std::uint64_t colorGeneration = 0;
    };
    std::vector<PointTarget> targets_;
};

} // namespace pci
