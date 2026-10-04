#include <pci/pointcloud/PointIdentity.h>

#include <atomic>
#include <limits>
#include <stdexcept>

namespace pci {

PointCloudSourceId allocatePointCloudSourceId()
{
    static std::atomic_uint64_t next = 1;
    auto value = next.load(std::memory_order_relaxed);
    for (;;) {
        if (value == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("point-cloud source ids are exhausted");
        }
        if (next.compare_exchange_weak(
                value, value + 1, std::memory_order_relaxed)) {
            return PointCloudSourceId{value};
        }
    }
}

} // namespace pci
