#pragma once

#include "pointcloud/PointColorPolicy.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pci {

enum class PointColorMapKind {
    Direct,
    Continuous,
    Categorical,
};

struct PointRgba {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    float alpha = 1.0F;

    bool operator==(const PointRgba &) const = default;
};

struct PointColorStop {
    float position = 0.0F;
    PointRgba color;
};

struct PointCategoricalColor {
    std::uint8_t value = 0;
    PointRgba color;
};

struct PointColorMapDefinition {
    PointColorMap id = PointColorMap::Rgb;
    std::string_view key;
    std::string_view name;
    std::string_view description;
    PointColorMapKind kind = PointColorMapKind::Direct;
    std::uint32_t compatibleSourceMask = 0;
    std::span<const PointColorStop> stops;
    std::span<const PointCategoricalColor> categoricalColors;
    PointRgba fallbackColor;
};

struct PointColorMapRegistrationResult {
    enum class Error {
        None,
        Frozen,
        EmptyKey,
        EmptyName,
        InvalidStops,
        IdCollision,
    };

    PointColorMap id = PointColorMap::Rgb;
    bool added = false;
    Error errorCode = Error::None;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error.empty();
    }
};

class PointColorMapCatalogSnapshot final {
public:
    PointColorMapCatalogSnapshot(const PointColorMapCatalogSnapshot &) = delete;
    PointColorMapCatalogSnapshot &
    operator=(const PointColorMapCatalogSnapshot &) = delete;
    ~PointColorMapCatalogSnapshot();

    [[nodiscard]] std::span<const PointColorMapDefinition>
    definitions() const noexcept;
    [[nodiscard]] const PointColorMapDefinition *
    definition(PointColorMap map) const noexcept;
    [[nodiscard]] bool supports(PointColorMap map,
                                PointColorSource source) const noexcept;
    [[nodiscard]] PointRgba sampleContinuous(PointColorMap map,
                                             float normalizedValue) const;
    [[nodiscard]] PointRgba sampleCategorical(PointColorMap map,
                                              std::uint8_t value) const;

private:
    friend class PointColorMapCatalog;
    struct Impl;
    explicit PointColorMapCatalogSnapshot(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

using PointColorMapCatalogSnapshotPtr =
    std::shared_ptr<const PointColorMapCatalogSnapshot>;

class PointColorMapCatalog final {
public:
    PointColorMapCatalog();
    ~PointColorMapCatalog();
    PointColorMapCatalog(PointColorMapCatalog &&) noexcept;
    PointColorMapCatalog &operator=(PointColorMapCatalog &&) noexcept;
    PointColorMapCatalog(const PointColorMapCatalog &) = delete;
    PointColorMapCatalog &operator=(const PointColorMapCatalog &) = delete;

    // IDs are deterministic for stable keys, so registration order does not
    // renumber existing selections.
    [[nodiscard]] PointColorMapRegistrationResult registerContinuous(
        std::string key,
        std::string name,
        std::vector<PointColorStop> stops,
        std::string description = {});
    [[nodiscard]] PointColorMapCatalogSnapshotPtr freeze();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] PointColorMapCatalogSnapshotPtr
createBuiltInPointColorMapCatalog();

[[nodiscard]] std::span<const PointColorMapDefinition>
pointColorMapCatalog(const PointColorMapCatalogSnapshot &catalog) noexcept;
[[nodiscard]] const PointColorMapDefinition *
pointColorMapDefinition(const PointColorMapCatalogSnapshot &catalog,
                        PointColorMap map) noexcept;
[[nodiscard]] bool
pointColorMapSupportsSource(const PointColorMapCatalogSnapshot &catalog,
                            PointColorMap map,
                            PointColorSource source);

// Continuous maps consume [0, 1]. Categorical maps consume the unscaled
// integer attribute value. These CPU samplers also build the GPU lookup atlas
// and make color-map behavior independently testable.
[[nodiscard]] PointRgba
sampleContinuousPointColorMap(const PointColorMapCatalogSnapshot &catalog,
                              PointColorMap map,
                              float normalizedValue);
[[nodiscard]] PointRgba
sampleCategoricalPointColorMap(const PointColorMapCatalogSnapshot &catalog,
                               PointColorMap map,
                               std::uint8_t value);

} // namespace pci
