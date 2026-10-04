#include <pci/color/PointColorMapCatalog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace pci {
namespace {

constexpr std::uint32_t sourceBit(const PointColorSource source) noexcept
{
    return std::uint32_t{1} << static_cast<std::uint32_t>(source);
}

constexpr std::uint32_t scalarSources =
    sourceBit(PointColorSource::X) | sourceBit(PointColorSource::Y) |
    sourceBit(PointColorSource::Z) | sourceBit(PointColorSource::Intensity);

constexpr std::array classificationColors{
    PointCategoricalColor{1, {0.55F, 0.55F, 0.55F, 1.0F}},
    PointCategoricalColor{2, {0.45F, 0.30F, 0.18F, 1.0F}},
    PointCategoricalColor{3, {0.20F, 0.65F, 0.20F, 1.0F}},
    PointCategoricalColor{4, {0.12F, 0.45F, 0.12F, 1.0F}},
    PointCategoricalColor{5, {0.05F, 0.80F, 0.05F, 1.0F}},
    PointCategoricalColor{6, {0.85F, 0.20F, 0.20F, 1.0F}},
    PointCategoricalColor{7, {0.85F, 0.85F, 0.85F, 1.0F}},
    PointCategoricalColor{9, {0.15F, 0.35F, 0.95F, 1.0F}},
};

constexpr std::array returnColors{
    PointCategoricalColor{1, {0.15F, 0.35F, 0.95F, 1.0F}},
    PointCategoricalColor{2, {0.15F, 0.75F, 0.25F, 1.0F}},
    PointCategoricalColor{3, {0.95F, 0.75F, 0.15F, 1.0F}},
    PointCategoricalColor{4, {0.90F, 0.30F, 0.20F, 1.0F}},
    PointCategoricalColor{5, {0.55F, 0.25F, 0.80F, 1.0F}},
};

constexpr std::array builtinDefinitions{
    PointColorMapDefinition{
        .id = PointColorMap::Rgb,
        .key = "builtin:rgb",
        .name = "RGB",
        .description = "Uses the RGB colors stored in the point cloud.",
        .kind = PointColorMapKind::Direct,
        .compatibleSourceMask = sourceBit(PointColorSource::Rgb),
        .stops = {},
        .categoricalColors = {},
        .fallbackColor = {},
    },
    PointColorMapDefinition{
        .id = PointColorMap::LasClassification,
        .key = "builtin:las-classification",
        .name = "LAS Classification",
        .description = "Uses distinct colors for LAS classification codes.",
        .kind = PointColorMapKind::Categorical,
        .compatibleSourceMask = sourceBit(PointColorSource::Classification),
        .stops = {},
        .categoricalColors = classificationColors,
        .fallbackColor = {0.95F, 0.95F, 0.95F, 1.0F},
    },
    PointColorMapDefinition{
        .id = PointColorMap::ReturnNumber,
        .key = "builtin:return-number",
        .name = "Return numbers",
        .description = "Uses distinct colors for point return numbers.",
        .kind = PointColorMapKind::Categorical,
        .compatibleSourceMask = sourceBit(PointColorSource::ReturnNumber) |
                                sourceBit(PointColorSource::NumberOfReturns),
        .stops = {},
        .categoricalColors = returnColors,
        .fallbackColor = {0.85F, 0.85F, 0.85F, 1.0F},
    },
};

struct OwnedContinuousPointColorMap {
    PointColorMap id = PointColorMap::Rgb;
    std::string key;
    std::string name;
    std::string description;
    std::vector<PointColorStop> stops;
};

constexpr std::string_view defaultContinuousDescription =
    "A continuous color map for ordered numeric values.";

PointColorMap dynamicMapId(const std::string_view key) noexcept
{
    if (key == "cpt:viridis.cpt") {
        return PointColorMap::Viridis;
    }
    if (key == "cpt:turbo.cpt") {
        return PointColorMap::Turbo;
    }
    constexpr std::uint32_t fnvOffset = 2166136261U;
    constexpr std::uint32_t fnvPrime = 16777619U;
    constexpr std::uint32_t dynamicIdBase = 1024U;
    constexpr std::uint32_t dynamicIdCount =
        static_cast<std::uint32_t>(std::numeric_limits<int>::max()) -
        dynamicIdBase;

    std::uint32_t hash = fnvOffset;
    for (const unsigned char byte : key) {
        hash ^= byte;
        hash *= fnvPrime;
    }
    return static_cast<PointColorMap>(
        static_cast<int>(dynamicIdBase + hash % dynamicIdCount));
}

bool finiteColor(const PointRgba color) noexcept
{
    const std::array components{
        color.red, color.green, color.blue, color.alpha};
    return std::ranges::all_of(components, [](const float component) {
        return std::isfinite(component) && component >= 0.0F &&
               component <= 1.0F;
    });
}

std::string validateStops(const std::span<const PointColorStop> stops)
{
    if (stops.size() < 2) {
        return "a continuous color map requires at least two stops";
    }
    float previous = -std::numeric_limits<float>::infinity();
    for (const PointColorStop &stop : stops) {
        if (!std::isfinite(stop.position) || stop.position < 0.0F ||
            stop.position > 1.0F) {
            return "color stop positions must be finite and in [0, 1]";
        }
        if (stop.position < previous) {
            return "color stop positions must be ordered";
        }
        if (!finiteColor(stop.color)) {
            return "color components must be finite and in [0, 1]";
        }
        previous = stop.position;
    }
    if (stops.front().position != 0.0F || stops.back().position != 1.0F) {
        return "continuous color maps must cover [0, 1]";
    }
    return {};
}

PointRgba clampColor(PointRgba color) noexcept
{
    color.red = std::clamp(color.red, 0.0F, 1.0F);
    color.green = std::clamp(color.green, 0.0F, 1.0F);
    color.blue = std::clamp(color.blue, 0.0F, 1.0F);
    color.alpha = std::clamp(color.alpha, 0.0F, 1.0F);
    return color;
}

} // namespace

struct PointColorMapCatalogSnapshot::Impl {
    std::deque<OwnedContinuousPointColorMap> ownedMaps;
    std::vector<PointColorMapDefinition> definitions;
};

struct PointColorMapCatalog::Impl {
    std::deque<OwnedContinuousPointColorMap> ownedMaps;
    PointColorMapCatalogSnapshotPtr snapshot;
};

PointColorMapCatalogSnapshot::PointColorMapCatalogSnapshot(
    std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

PointColorMapCatalogSnapshot::~PointColorMapCatalogSnapshot() = default;

std::span<const PointColorMapDefinition>
PointColorMapCatalogSnapshot::definitions() const noexcept
{
    return impl_->definitions;
}

const PointColorMapDefinition *
PointColorMapCatalogSnapshot::definition(const PointColorMap map) const noexcept
{
    const auto found = std::ranges::find(
        impl_->definitions, map, &PointColorMapDefinition::id);
    return found == impl_->definitions.end() ? nullptr : &*found;
}

bool PointColorMapCatalogSnapshot::supports(
    const PointColorMap map, const PointColorSource source) const noexcept
{
    const PointColorMapDefinition *found = definition(map);
    return found && (found->compatibleSourceMask & sourceBit(source)) != 0;
}

PointRgba PointColorMapCatalogSnapshot::sampleContinuous(
    const PointColorMap map, const float normalizedValue) const
{
    const PointColorMapDefinition *found = definition(map);
    if (!found || found->kind != PointColorMapKind::Continuous) {
        return {};
    }
    const float value = std::clamp(normalizedValue, 0.0F, 1.0F);
    return clampColor(interpolateColorRamp(found->stops, value, {}));
}

PointRgba
PointColorMapCatalogSnapshot::sampleCategorical(const PointColorMap map,
                                                const std::uint8_t value) const
{
    const PointColorMapDefinition *found = definition(map);
    if (!found || found->kind != PointColorMapKind::Categorical) {
        return {};
    }
    const auto color = std::ranges::find(
        found->categoricalColors, value, &PointCategoricalColor::value);
    return color == found->categoricalColors.end() ? found->fallbackColor
                                                   : color->color;
}

PointColorMapCatalog::PointColorMapCatalog()
    : impl_(std::make_unique<Impl>())
{
}

PointColorMapCatalog::~PointColorMapCatalog() = default;
PointColorMapCatalog::PointColorMapCatalog(PointColorMapCatalog &&) noexcept =
    default;
PointColorMapCatalog &
PointColorMapCatalog::operator=(PointColorMapCatalog &&) noexcept = default;

PointColorMapRegistrationResult
PointColorMapCatalog::registerContinuous(std::string key,
                                         std::string name,
                                         std::vector<PointColorStop> stops,
                                         std::string description)
{
    if (impl_->snapshot) {
        return {
            .errorCode = PointColorMapRegistrationResult::Error::Frozen,
            .error = "the point color-map catalog has already been frozen",
        };
    }
    if (key.empty()) {
        return {
            .errorCode = PointColorMapRegistrationResult::Error::EmptyKey,
            .error = "a point color map requires a stable key",
        };
    }
    if (name.empty()) {
        return {
            .errorCode = PointColorMapRegistrationResult::Error::EmptyName,
            .error = "a point color map requires a display name",
        };
    }
    if (const std::string validation = validateStops(stops);
        !validation.empty()) {
        return {
            .errorCode = PointColorMapRegistrationResult::Error::InvalidStops,
            .error = validation,
        };
    }
    if (description.empty()) {
        description = defaultContinuousDescription;
    }

    const auto builtinKey = std::ranges::find(
        builtinDefinitions, key, &PointColorMapDefinition::key);
    if (builtinKey != builtinDefinitions.end()) {
        return {
            .id = builtinKey->id,
            .added = false,
            .errorCode = PointColorMapRegistrationResult::Error::None,
            .error = {},
        };
    }
    const auto existingKey = std::ranges::find(
        impl_->ownedMaps, key, &OwnedContinuousPointColorMap::key);
    if (existingKey != impl_->ownedMaps.end()) {
        return {
            .id = existingKey->id,
            .added = false,
            .errorCode = PointColorMapRegistrationResult::Error::None,
            .error = {},
        };
    }

    const PointColorMap id = dynamicMapId(key);
    const auto builtinId =
        std::ranges::find(builtinDefinitions, id, &PointColorMapDefinition::id);
    const auto existingId = std::ranges::find(
        impl_->ownedMaps, id, &OwnedContinuousPointColorMap::id);
    if (builtinId != builtinDefinitions.end() ||
        existingId != impl_->ownedMaps.end()) {
        const std::string_view existingKeyValue =
            builtinId != builtinDefinitions.end() ? builtinId->key
                                                  : existingId->key;
        return {
            .errorCode = PointColorMapRegistrationResult::Error::IdCollision,
            .error = "stable point color-map ID collision between '" + key +
                     "' and '" + std::string(existingKeyValue) + "'",
        };
    }

    impl_->ownedMaps.push_back({
        .id = id,
        .key = std::move(key),
        .name = std::move(name),
        .description = std::move(description),
        .stops = std::move(stops),
    });
    return {
        .id = id,
        .added = true,
        .errorCode = PointColorMapRegistrationResult::Error::None,
        .error = {},
    };
}

PointColorMapCatalogSnapshotPtr PointColorMapCatalog::freeze()
{
    if (impl_->snapshot) {
        return impl_->snapshot;
    }
    auto snapshot = std::make_unique<PointColorMapCatalogSnapshot::Impl>();
    snapshot->ownedMaps = impl_->ownedMaps;
    snapshot->definitions.assign(builtinDefinitions.begin(),
                                 builtinDefinitions.end());
    snapshot->definitions.reserve(builtinDefinitions.size() +
                                  snapshot->ownedMaps.size());
    for (const OwnedContinuousPointColorMap &owned : snapshot->ownedMaps) {
        snapshot->definitions.push_back({
            .id = owned.id,
            .key = owned.key,
            .name = owned.name,
            .description = owned.description,
            .kind = PointColorMapKind::Continuous,
            .compatibleSourceMask = scalarSources,
            .stops = owned.stops,
            .categoricalColors = {},
            .fallbackColor = {},
        });
    }
    impl_->snapshot = PointColorMapCatalogSnapshotPtr(
        new PointColorMapCatalogSnapshot(std::move(snapshot)));
    return impl_->snapshot;
}

PointColorMapCatalogSnapshotPtr createBuiltInPointColorMapCatalog()
{
    PointColorMapCatalog catalog;
    return catalog.freeze();
}

std::span<const PointColorMapDefinition>
pointColorMapCatalog(const PointColorMapCatalogSnapshot &catalog) noexcept
{
    return catalog.definitions();
}

const PointColorMapDefinition *
pointColorMapDefinition(const PointColorMapCatalogSnapshot &catalog,
                        const PointColorMap map) noexcept
{
    return catalog.definition(map);
}

bool pointColorMapSupportsSource(const PointColorMapCatalogSnapshot &catalog,
                                 const PointColorMap map,
                                 const PointColorSource source)
{
    return catalog.supports(map, source);
}

PointRgba
sampleContinuousPointColorMap(const PointColorMapCatalogSnapshot &catalog,
                              const PointColorMap map,
                              const float normalizedValue)
{
    return catalog.sampleContinuous(map, normalizedValue);
}

PointRgba
sampleCategoricalPointColorMap(const PointColorMapCatalogSnapshot &catalog,
                               const PointColorMap map,
                               const std::uint8_t value)
{
    return catalog.sampleCategorical(map, value);
}

} // namespace pci
