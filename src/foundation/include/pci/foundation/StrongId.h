#pragma once

#include <pci/foundation/Hash.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

namespace pci {

template <typename Tag, typename Representation = std::uint64_t>
class StrongId {
public:
    using representation_type = Representation;

    constexpr StrongId() = default;

    explicit constexpr StrongId(Representation value) noexcept(
        std::is_nothrow_move_constructible_v<Representation>)
        : value_(std::move(value))
    {
    }

    [[nodiscard]] constexpr const Representation &value() const noexcept
    {
        return value_;
    }

    auto operator<=>(const StrongId &) const = default;

private:
    Representation value_{};
};

} // namespace pci

namespace std {

// Specializing std::hash for a program-defined type is permitted; clang-tidy 22
// misclassifies this dependent partial specialization.
template <typename Tag, typename Representation>
// NOLINTNEXTLINE(bugprone-std-namespace-modification)
struct hash<pci::StrongId<Tag, Representation>> {
    [[nodiscard]] std::size_t
    operator()(const pci::StrongId<Tag, Representation> &id) const
        noexcept(noexcept(std::hash<Representation>{}(id.value())))
    {
        return pci::hashCombine(0, std::hash<Representation>{}(id.value()));
    }
};

} // namespace std
