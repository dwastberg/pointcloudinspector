#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

namespace pci {

[[nodiscard]] std::size_t
grownUniformDrawCapacity(std::size_t currentCapacity,
                         std::size_t requiredDrawCount) noexcept;

// Internal numeric seam used to validate an upload size without constructing
// a span over memory that does not exist.
[[nodiscard]] std::size_t
checkedUniformStagingByteSize(std::size_t count,
                              std::size_t stride,
                              std::size_t uniformSize,
                              std::size_t maximumOutputBytes,
                              const char *strideError,
                              const char *sizeError);

template <typename Draw, typename Projection>
    requires std::invocable<Projection, const Draw &> &&
             std::is_lvalue_reference_v<
                 std::invoke_result_t<Projection, const Draw &>> &&
             std::is_const_v<std::remove_reference_t<
                 std::invoke_result_t<Projection, const Draw &>>> &&
             std::is_trivially_copyable_v<std::remove_cvref_t<
                 std::invoke_result_t<Projection, const Draw &>>>
void stageUniformRecords(const std::span<const Draw> draws,
                         const std::size_t stride,
                         Projection projection,
                         std::vector<std::byte> &output,
                         const char *strideError,
                         const char *sizeError,
                         const std::size_t maximumOutputBytes =
                             std::numeric_limits<std::size_t>::max())
{
    using Uniform =
        std::remove_cvref_t<std::invoke_result_t<Projection, const Draw &>>;
    const std::size_t byteSize = checkedUniformStagingByteSize(
        draws.size(),
        stride,
        sizeof(Uniform),
        std::min(output.max_size(), maximumOutputBytes),
        strideError,
        sizeError);

    output.resize(byteSize);
    std::ranges::fill(output, std::byte{});
    for (std::size_t index = 0; index < draws.size(); ++index) {
        const Uniform &uniform = std::invoke(projection, draws[index]);
        std::memcpy(output.data() + index * stride, &uniform, sizeof(Uniform));
    }
}

} // namespace pci
