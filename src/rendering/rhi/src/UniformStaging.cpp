#include <pci/rendering/rhi/UniformStaging.h>

#include <pci/foundation/CheckedArithmetic.h>

#include <limits>
#include <stdexcept>

namespace pci {

std::size_t
grownUniformDrawCapacity(const std::size_t currentCapacity,
                         const std::size_t requiredDrawCount) noexcept
{
    if (requiredDrawCount <= currentCapacity) {
        return currentCapacity;
    }
    if (currentCapacity == 0) {
        return requiredDrawCount;
    }

    std::size_t capacity = currentCapacity;
    while (capacity < requiredDrawCount) {
        if (capacity > std::numeric_limits<std::size_t>::max() / 2) {
            return requiredDrawCount;
        }
        capacity *= 2;
    }
    return capacity;
}

std::size_t checkedUniformStagingByteSize(const std::size_t count,
                                          const std::size_t stride,
                                          const std::size_t uniformSize,
                                          const std::size_t maximumOutputBytes,
                                          const char *strideError,
                                          const char *sizeError)
{
    if (stride < uniformSize) {
        throw std::invalid_argument(strideError);
    }

    const std::optional<std::size_t> byteSize = checkedMultiply(count, stride);
    if (!byteSize || *byteSize > maximumOutputBytes) {
        throw std::length_error(sizeError);
    }
    return *byteSize;
}

} // namespace pci
