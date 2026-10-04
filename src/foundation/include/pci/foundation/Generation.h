#pragma once

#include <pci/foundation/StrongId.h>

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace pci {

struct SessionGenerationTag;
struct DocumentGenerationTag;
struct BindingGenerationTag;
struct AttemptGenerationTag;

using SessionGeneration = StrongId<SessionGenerationTag>;
using DocumentGeneration = StrongId<DocumentGenerationTag>;
using BindingGeneration = StrongId<BindingGenerationTag>;
using AttemptGeneration = StrongId<AttemptGenerationTag>;

namespace detail {

template <typename Generation>
[[nodiscard]] constexpr Generation
checkedNextGeneration(const Generation current)
{
    if (current.value() == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("generation values are exhausted");
    }
    return Generation{current.value() + 1U};
}

} // namespace detail

[[nodiscard]] constexpr SessionGeneration
nextGeneration(const SessionGeneration current)
{
    return detail::checkedNextGeneration(current);
}

[[nodiscard]] constexpr DocumentGeneration
nextGeneration(const DocumentGeneration current)
{
    return detail::checkedNextGeneration(current);
}

[[nodiscard]] constexpr BindingGeneration
nextGeneration(const BindingGeneration current)
{
    return detail::checkedNextGeneration(current);
}

[[nodiscard]] constexpr AttemptGeneration
nextGeneration(const AttemptGeneration current)
{
    return detail::checkedNextGeneration(current);
}

} // namespace pci
