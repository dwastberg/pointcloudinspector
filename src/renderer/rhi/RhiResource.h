#pragma once

#include <rhi/qrhi.h>

#include <memory>

namespace pci {

template <typename Resource> using RhiResourcePtr = std::unique_ptr<Resource>;

template <typename Resource> struct RhiReleaseDeleter {
    void operator()(Resource *resource) const noexcept
    {
        if (resource) {
            resource->release();
        }
    }
};

using RhiResourceUpdateBatchPtr =
    std::unique_ptr<QRhiResourceUpdateBatch,
                    RhiReleaseDeleter<QRhiResourceUpdateBatch>>;

} // namespace pci
