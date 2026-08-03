# Known Point Color Maps Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add first-pass point-cloud color maps for RGB, X, Y, Z, intensity, classification, return number, and number of returns without increasing the per-point GPU stride or re-uploading points when color mode changes.

**Architecture:** Keep `GpuPoint` at 16 bytes and rename its unused 32-bit lane to `packedProperties`. Use that lane for full intensity and return attributes, expose a small renderer color-mode contract, and let the Metal display shader derive color from a uniform-selected source while the picker path continues to read only position and vertex ID.

**Tech Stack:** C++23, Qt Widgets, Qt QRhi/Metal, Qt Shader Tools GLSL-to-QSB, PDAL import tests, existing no-framework C++ test executables.

---

## File Structure

- Create `src/core/GpuPointProperties.h`: constexpr packing and unpacking helpers for the known per-point properties carried in `GpuPoint::packedProperties`.
- Modify `src/core/GpuPoint.h`: rename `padding` to `packedProperties` while preserving the 16-byte binary layout.
- Modify `src/core/SyntheticPointCloud.cpp`: initialize `packedProperties` for generated points.
- Modify `src/import/pdal/PdalPointCloudLoader.cpp`: pack full intensity, return number, and number of returns into `GpuPoint`.
- Modify `tests/pointcloud_contract_tests.cpp`: verify the compact point layout and property packing helpers.
- Modify `tests/pdal_import_tests.cpp`: verify imported render points carry the same known properties as the CPU-side attribute stream.
- Create `src/renderer/PointColorMode.h`: renderer-facing color source enum, mode struct, source availability helper, default-mode helper, and labels.
- Modify `src/renderer/RenderViewport.h`: add color-mode methods to the common renderer interface.
- Modify `src/renderer/metal/MetalRenderViewport.{h,cpp}`: store current color mode, expose available sources, select defaults on load, update the color uniform, and add the packed property vertex attribute.
- Modify `shaders/points.vert`: compute display color from the selected source.
- Modify `shaders/pick.vert`: keep the camera uniform layout compatible with the expanded display uniform.
- Modify `tests/renderer_contract_tests.cpp`: verify color source availability, default selection, mode changes, and invalid mode rejection.
- Modify `src/app/MainWindow.{h,cpp}`: add a toolbar color-source combo box wired to the renderer.
- Modify `tests/main_window_tests.cpp`: verify the color-source selector is populated and calls the renderer without invoking loading progress.

---

### Task 1: Pack known LAS properties into the existing 16-byte point record

**Files:**
- Create: `src/core/GpuPointProperties.h`
- Modify: `src/core/GpuPoint.h`
- Modify: `src/core/SyntheticPointCloud.cpp`
- Modify: `src/import/pdal/PdalPointCloudLoader.cpp`
- Test: `tests/pointcloud_contract_tests.cpp`
- Test: `tests/pdal_import_tests.cpp`

- [ ] **Step 1: Write the failing point packing contract tests**

Modify `tests/pointcloud_contract_tests.cpp`.

Add this include beside the existing core/pointcloud includes:

```cpp
#include "core/GpuPointProperties.h"
```

In `testSourcePointDefaultsAreRenderable()`, replace:

```cpp
    CHECK(point.padding == 0);
```

with:

```cpp
    CHECK(point.packedProperties == 0);
```

Add this test function before `main()`:

```cpp
void testGpuPointPropertyPackingPreservesKnownFields()
{
    static_assert(sizeof(pci::GpuPoint) == 16);

    const std::uint32_t packed =
        pci::packGpuPointProperties(513, 7, 9);
    CHECK(pci::gpuPointIntensity(packed) == 513);
    CHECK(pci::gpuPointReturnNumber(packed) == 7);
    CHECK(pci::gpuPointNumberOfReturns(packed) == 9);

    const std::uint32_t saturated =
        pci::packGpuPointProperties(65535, 255, 255);
    CHECK(pci::gpuPointIntensity(saturated) == 65535);
    CHECK(pci::gpuPointReturnNumber(saturated) == 255);
    CHECK(pci::gpuPointNumberOfReturns(saturated) == 255);
}
```

Call the new test from `main()` after `testPointAttributesDefaultsAreNeutral();`:

```cpp
        testGpuPointPropertyPackingPreservesKnownFields();
```

- [ ] **Step 2: Write the failing PDAL import packing checks**

Modify `tests/pdal_import_tests.cpp`.

Add this include:

```cpp
#include "core/GpuPointProperties.h"
```

In the test that checks the first imported LAS point, after:

```cpp
    CHECK(las->attributes.front().numberOfReturns == 2);
```

add:

```cpp
    CHECK(pci::gpuPointIntensity(las->points.front().packedProperties) == 100);
    CHECK(pci::gpuPointReturnNumber(las->points.front().packedProperties) == 1);
    CHECK(pci::gpuPointNumberOfReturns(las->points.front().packedProperties) == 2);
```

In `testPointImportIsDeterministicAcrossRuns()`, after:

```cpp
    CHECK(first->attributes[1].numberOfReturns == 3);
```

add:

```cpp
    CHECK(pci::gpuPointIntensity(first->points[0].packedProperties) == 100);
    CHECK(pci::gpuPointReturnNumber(first->points[0].packedProperties) == 1);
    CHECK(pci::gpuPointNumberOfReturns(first->points[0].packedProperties) == 2);
    CHECK(pci::gpuPointIntensity(first->points[1].packedProperties) == 500);
    CHECK(pci::gpuPointReturnNumber(first->points[1].packedProperties) == 2);
    CHECK(pci::gpuPointNumberOfReturns(first->points[1].packedProperties) == 3);
```

- [ ] **Step 3: Run the focused tests and verify they fail**

Run:

```bash
cmake --build build --target pcinspector_pointcloud_contract_tests pcinspector_pdal_import_tests
ctest --test-dir build --output-on-failure -R '^(pointcloud_contract|pdal_import)$'
```

Expected: build fails because `core/GpuPointProperties.h`, `GpuPoint::packedProperties`, and the packing helper functions do not exist.

- [ ] **Step 4: Add the packing helpers**

Create `src/core/GpuPointProperties.h`:

```cpp
#pragma once

#include <cstdint>

namespace pci {

constexpr std::uint32_t packGpuPointProperties(
    const std::uint16_t intensity,
    const std::uint8_t returnNumber,
    const std::uint8_t numberOfReturns) noexcept
{
    return static_cast<std::uint32_t>(intensity)
        | (static_cast<std::uint32_t>(returnNumber) << 16U)
        | (static_cast<std::uint32_t>(numberOfReturns) << 24U);
}

constexpr std::uint16_t gpuPointIntensity(
    const std::uint32_t packedProperties) noexcept
{
    return static_cast<std::uint16_t>(packedProperties & 0xffffU);
}

constexpr std::uint8_t gpuPointReturnNumber(
    const std::uint32_t packedProperties) noexcept
{
    return static_cast<std::uint8_t>((packedProperties >> 16U) & 0xffU);
}

constexpr std::uint8_t gpuPointNumberOfReturns(
    const std::uint32_t packedProperties) noexcept
{
    return static_cast<std::uint8_t>((packedProperties >> 24U) & 0xffU);
}

} // namespace pci
```

- [ ] **Step 5: Rename the point lane**

Modify `src/core/GpuPoint.h`:

```cpp
struct GpuPoint {
    std::uint16_t x;
    std::uint16_t y;
    std::uint16_t z;
    std::uint16_t attributes;
    std::uint32_t rgba;
    std::uint32_t packedProperties;

    bool operator==(const GpuPoint &) const = default;
};
```

Keep:

```cpp
static_assert(sizeof(GpuPoint) == 16);
```

- [ ] **Step 6: Update existing initializers**

Run:

```bash
rg -n "padding" src tests
```

For each `GpuPoint` initializer reported by that command, replace `.padding = 0,` with:

```cpp
        .packedProperties = 0,
```

Expected files include `src/core/SyntheticPointCloud.cpp`, `tests/metal_point_picker_tests.cpp`, and any existing test fixture that constructs a `GpuPoint` directly.

- [ ] **Step 7: Pack PDAL properties into render points**

Modify `src/import/pdal/PdalPointCloudLoader.cpp`.

Add the helper include:

```cpp
#include "core/GpuPointProperties.h"
```

In `mapPoint()`, keep the existing `classification` and `intensity` local variables, then add:

```cpp
    const auto returnNumber = metadata.hasReturnNumber
        ? point.getFieldAs<std::uint8_t>(
              pdal::Dimension::Id::ReturnNumber)
        : std::uint8_t{0};
    const auto numberOfReturns = metadata.hasNumberOfReturns
        ? point.getFieldAs<std::uint8_t>(
              pdal::Dimension::Id::NumberOfReturns)
        : std::uint8_t{0};
```

In the returned `GpuPoint`, replace `.padding = 0,` with:

```cpp
        .packedProperties = packGpuPointProperties(
            intensity, returnNumber, numberOfReturns),
```

- [ ] **Step 8: Run the focused tests and verify they pass**

Run:

```bash
cmake --build build --target pcinspector_pointcloud_contract_tests pcinspector_pdal_import_tests
ctest --test-dir build --output-on-failure -R '^(pointcloud_contract|pdal_import)$'
```

Expected: both tests pass.

- [ ] **Step 9: Commit the packed property lane**

Run:

```bash
git add src/core/GpuPointProperties.h src/core/GpuPoint.h src/core/SyntheticPointCloud.cpp src/import/pdal/PdalPointCloudLoader.cpp tests/pointcloud_contract_tests.cpp tests/pdal_import_tests.cpp tests/metal_point_picker_tests.cpp
git commit -m "feat: pack known point properties for color maps"
```

---

### Task 2: Add the renderer color-mode contract

**Files:**
- Create: `src/renderer/PointColorMode.h`
- Modify: `src/renderer/RenderViewport.h`
- Modify: `src/renderer/metal/MetalRenderViewport.h`
- Modify: `src/renderer/metal/MetalRenderViewport.cpp`
- Test: `tests/renderer_contract_tests.cpp`
- Test: `tests/main_window_tests.cpp`

- [ ] **Step 1: Write failing renderer contract tests for color sources**

Modify `tests/renderer_contract_tests.cpp`.

Add this include:

```cpp
#include "renderer/PointColorMode.h"
```

Add standard library includes if they are not already present:

```cpp
#include <algorithm>
#include <vector>
```

Add this helper near the existing helpers:

```cpp
bool hasSource(const std::vector<pci::PointColorSource> &sources,
               const pci::PointColorSource source)
{
    return std::ranges::find(sources, source) != sources.end();
}
```

In the loaded cloud setup, before `loaded->points.resize(2);`, add:

```cpp
        loaded->metadata.hasColor = true;
        loaded->metadata.hasIntensity = true;
        loaded->metadata.hasClassification = true;
        loaded->metadata.hasReturnNumber = true;
        loaded->metadata.hasNumberOfReturns = true;
```

After `CHECK(viewport->totalPointCount() == 2);`, add:

```cpp
        const auto sources = viewport->availableColorSources();
        CHECK(hasSource(sources, pci::PointColorSource::Rgb));
        CHECK(hasSource(sources, pci::PointColorSource::X));
        CHECK(hasSource(sources, pci::PointColorSource::Y));
        CHECK(hasSource(sources, pci::PointColorSource::Z));
        CHECK(hasSource(sources, pci::PointColorSource::Intensity));
        CHECK(hasSource(sources, pci::PointColorSource::Classification));
        CHECK(hasSource(sources, pci::PointColorSource::ReturnNumber));
        CHECK(hasSource(sources, pci::PointColorSource::NumberOfReturns));
        CHECK(viewport->colorMode().source == pci::PointColorSource::Rgb);

        viewport->setColorMode({.source = pci::PointColorSource::Z});
        CHECK(viewport->colorMode().source == pci::PointColorSource::Z);

        auto noIntensity = std::make_shared<pci::LoadedPointCloud>();
        noIntensity->metadata.sourcePointCount = 1;
        noIntensity->points.resize(1);
        viewport->setPointCloud(noIntensity);
        CHECK(viewport->colorMode().source == pci::PointColorSource::Z);
        viewport->setColorMode({.source = pci::PointColorSource::Intensity});
        CHECK(viewport->colorMode().source == pci::PointColorSource::Z);
```

- [ ] **Step 2: Update the main-window fake viewport to expose the new interface**

Modify `tests/main_window_tests.cpp`.

Add this include:

```cpp
#include "renderer/PointColorMode.h"
```

In `FakeViewport`, add these methods after `setPointCloud()`:

```cpp
    void setColorMode(const pci::PointColorMode mode) override
    {
        if (pci::pointColorSourceAvailable(
                availableColorSources(), mode.source)) {
            colorMode_ = mode;
        }
    }

    pci::PointColorMode colorMode() const noexcept override
    {
        return colorMode_;
    }

    std::vector<pci::PointColorSource>
    availableColorSources() const override
    {
        return pci::availablePointColorSources(metadata_);
    }
```

Change `FakeViewport::setPointCloud()` to:

```cpp
    void setPointCloud(pci::LoadedPointCloudPtr cloud) override
    {
        metadata_ = cloud->metadata;
        pointCount_ = cloud->points.size();
        colorMode_ = pci::defaultPointColorMode(metadata_);
    }
```

Add these private fields:

```cpp
    pci::PointCloudMetadata metadata_;
    pci::PointColorMode colorMode_;
```

- [ ] **Step 3: Run the focused tests and verify they fail**

Run:

```bash
cmake --build build --target pcinspector_renderer_contract_tests pcinspector_main_window_tests
ctest --test-dir build --output-on-failure -R '^(renderer_contract|main_window)$'
```

Expected: build fails because `renderer/PointColorMode.h` and the new `RenderViewport` virtual methods do not exist.

- [ ] **Step 4: Add the color-mode header**

Create `src/renderer/PointColorMode.h`:

```cpp
#pragma once

#include "pointcloud/PointCloudMetadata.h"

#include <QString>

#include <algorithm>
#include <vector>

namespace pci {

enum class PointColorSource : int {
    Rgb = 0,
    X = 1,
    Y = 2,
    Z = 3,
    Intensity = 4,
    Classification = 5,
    ReturnNumber = 6,
    NumberOfReturns = 7,
};

struct PointColorMode {
    PointColorSource source = PointColorSource::Rgb;

    bool operator==(const PointColorMode &) const = default;
};

inline std::vector<PointColorSource>
availablePointColorSources(const PointCloudMetadata &metadata)
{
    std::vector<PointColorSource> sources;
    if (metadata.hasColor) {
        sources.push_back(PointColorSource::Rgb);
    }
    sources.push_back(PointColorSource::X);
    sources.push_back(PointColorSource::Y);
    sources.push_back(PointColorSource::Z);
    if (metadata.hasIntensity) {
        sources.push_back(PointColorSource::Intensity);
    }
    if (metadata.hasClassification) {
        sources.push_back(PointColorSource::Classification);
    }
    if (metadata.hasReturnNumber) {
        sources.push_back(PointColorSource::ReturnNumber);
    }
    if (metadata.hasNumberOfReturns) {
        sources.push_back(PointColorSource::NumberOfReturns);
    }
    return sources;
}

inline bool pointColorSourceAvailable(
    const std::vector<PointColorSource> &sources,
    const PointColorSource source)
{
    return std::ranges::find(sources, source) != sources.end();
}

inline PointColorMode defaultPointColorMode(
    const PointCloudMetadata &metadata) noexcept
{
    return {
        .source = metadata.hasColor
            ? PointColorSource::Rgb
            : PointColorSource::Z,
    };
}

inline QString pointColorSourceLabel(const PointColorSource source)
{
    switch (source) {
    case PointColorSource::Rgb:
        return QStringLiteral("RGB");
    case PointColorSource::X:
        return QStringLiteral("X");
    case PointColorSource::Y:
        return QStringLiteral("Y");
    case PointColorSource::Z:
        return QStringLiteral("Z");
    case PointColorSource::Intensity:
        return QStringLiteral("Intensity");
    case PointColorSource::Classification:
        return QStringLiteral("Classification");
    case PointColorSource::ReturnNumber:
        return QStringLiteral("Return number");
    case PointColorSource::NumberOfReturns:
        return QStringLiteral("Number of returns");
    }
    return QStringLiteral("Unknown");
}

} // namespace pci
```

- [ ] **Step 5: Extend the render viewport interface**

Modify `src/renderer/RenderViewport.h`.

Add:

```cpp
#include "renderer/PointColorMode.h"
```

Add:

```cpp
#include <vector>
```

Add these pure virtual methods after `setPointCloud()`:

```cpp
    virtual void setColorMode(PointColorMode mode) = 0;
    [[nodiscard]] virtual PointColorMode colorMode() const noexcept = 0;
    [[nodiscard]] virtual std::vector<PointColorSource>
    availableColorSources() const = 0;
```

- [ ] **Step 6: Add color-mode state to the Metal viewport**

Modify `src/renderer/metal/MetalRenderViewport.h`.

Add overrides after `setPointCloud(...)`:

```cpp
    void setColorMode(PointColorMode mode) override;
    [[nodiscard]] PointColorMode colorMode() const noexcept override;
    [[nodiscard]] std::vector<PointColorSource>
    availableColorSources() const override;
```

Add this private member near `LoadedPointCloudPtr pointCloud_;`:

```cpp
    PointCloudMetadata activeMetadata_;
    PointColorMode colorMode_;
```

Modify `src/renderer/metal/MetalRenderViewport.cpp`.

In the constructor body, initialize synthetic color availability after `setFocusPolicy(Qt::StrongFocus);`:

```cpp
    activeMetadata_.hasColor = true;
    colorMode_ = {.source = PointColorSource::Rgb};
```

In `setPointCloud(...)`, after `pointCloud_ = std::move(cloud);`, add:

```cpp
    activeMetadata_ = pointCloud_->metadata;
    colorMode_ = defaultPointColorMode(activeMetadata_);
```

Add these methods after `setPointCloud(...)`:

```cpp
void MetalRenderViewport::setColorMode(const PointColorMode mode)
{
    if (!pointColorSourceAvailable(availableColorSources(), mode.source)) {
        qWarning().noquote()
            << QStringLiteral("Point color source is unavailable: %1")
                   .arg(pointColorSourceLabel(mode.source));
        return;
    }
    if (colorMode_ == mode) {
        return;
    }
    colorMode_ = mode;
    update();
}

PointColorMode MetalRenderViewport::colorMode() const noexcept
{
    return colorMode_;
}

std::vector<PointColorSource>
MetalRenderViewport::availableColorSources() const
{
    return availablePointColorSources(activeMetadata_);
}
```

- [ ] **Step 7: Run focused tests and verify they pass**

Run:

```bash
cmake --build build --target pcinspector_renderer_contract_tests pcinspector_main_window_tests
ctest --test-dir build --output-on-failure -R '^(renderer_contract|main_window)$'
```

Expected: both tests pass.

- [ ] **Step 8: Commit the color-mode contract**

Run:

```bash
git add src/renderer/PointColorMode.h src/renderer/RenderViewport.h src/renderer/metal/MetalRenderViewport.h src/renderer/metal/MetalRenderViewport.cpp tests/renderer_contract_tests.cpp tests/main_window_tests.cpp
git commit -m "feat: add point color mode contract"
```

---

### Task 3: Apply color maps in the Metal display shader

**Files:**
- Modify: `src/renderer/metal/MetalRenderViewport.h`
- Modify: `src/renderer/metal/MetalRenderViewport.cpp`
- Modify: `shaders/points.vert`
- Modify: `shaders/pick.vert`
- Test: `tests/renderer_contract_tests.cpp`
- Test: `tests/metal_point_picker_tests.cpp`

- [ ] **Step 1: Add a renderer contract assertion that color-mode changes do not reload points**

Modify `tests/renderer_contract_tests.cpp`.

After:

```cpp
        viewport->setColorMode({.source = pci::PointColorSource::Z});
        CHECK(viewport->colorMode().source == pci::PointColorSource::Z);
```

add:

```cpp
        bool sawReloadAfterColorChange = false;
        viewport->setLoadProgressCallback(
            [&sawReloadAfterColorChange](
                const pci::RenderLoadProgress &) {
                sawReloadAfterColorChange = true;
            });
        viewport->setColorMode({.source = pci::PointColorSource::Intensity});
        CHECK(viewport->colorMode().source == pci::PointColorSource::Intensity);
        QCoreApplication::processEvents();
        CHECK(!sawReloadAfterColorChange);
```

- [ ] **Step 2: Run the focused renderer contract test**

Run:

```bash
cmake --build build --target pcinspector_renderer_contract_tests
ctest --test-dir build --output-on-failure -R '^renderer_contract$'
```

Expected: test passes with the Task 2 implementation. This confirms the no-reload behavior before touching shaders.

- [ ] **Step 3: Extend the CPU uniform using existing padding bytes**

Modify `src/renderer/metal/MetalRenderViewport.h`.

Add:

```cpp
#include <cstdint>
```

Replace `CameraUniform` with:

```cpp
    struct alignas(16) CameraUniform {
        float mvp[16]{};
        float pointSize = 1.0F;
        std::int32_t colorSource = 0;
        float scalarMinimum = 0.0F;
        float scalarMaximum = 1.0F;
    };
    static_assert(sizeof(CameraUniform) == 80);
```

- [ ] **Step 4: Fill the color uniform**

Modify `MetalRenderViewport::cameraUniform()` in `src/renderer/metal/MetalRenderViewport.cpp`.

After:

```cpp
    uniform.pointSize = 2.0F;
```

add:

```cpp
    uniform.colorSource = static_cast<std::int32_t>(colorMode_.source);
    uniform.scalarMinimum = 0.0F;
    uniform.scalarMaximum = 65535.0F;
```

- [ ] **Step 5: Add the packed property vertex attribute**

Modify `MetalRenderViewport::createPipeline()` in `src/renderer/metal/MetalRenderViewport.cpp`.

Replace the display input attributes with:

```cpp
    inputLayout.setAttributes({
        QRhiVertexInputAttribute(
            0, 0, QRhiVertexInputAttribute::UShort4, 0),
        QRhiVertexInputAttribute(
            0, 1, QRhiVertexInputAttribute::UNormByte4, 8),
        QRhiVertexInputAttribute(
            0, 2, QRhiVertexInputAttribute::UInt, 12),
    });
```

Do not change `MetalPointPicker::ensureResources()`. The picker pipeline should continue to bind only location 0.

- [ ] **Step 6: Update the display shader**

Replace `shaders/points.vert` with:

```glsl
#version 450

layout(location = 0) in uvec4 positionAttributes;
layout(location = 1) in vec4 color;
layout(location = 2) in uint packedProperties;

layout(binding = 0) uniform Camera {
    mat4 mvp;
    float pointSize;
    int colorSource;
    float scalarMinimum;
    float scalarMaximum;
} camera;

layout(location = 0) out vec4 pointColor;

const int ColorRgb = 0;
const int ColorX = 1;
const int ColorY = 2;
const int ColorZ = 3;
const int ColorIntensity = 4;
const int ColorClassification = 5;
const int ColorReturnNumber = 6;
const int ColorNumberOfReturns = 7;

float normalizedScalar(float value)
{
    float span = max(camera.scalarMaximum - camera.scalarMinimum, 1.0);
    return clamp((value - camera.scalarMinimum) / span, 0.0, 1.0);
}

vec3 scalarColor(float value)
{
    float t = normalizedScalar(value);
    return vec3(t);
}

vec3 classificationColor(uint classification)
{
    switch (classification) {
    case 1u:
        return vec3(0.55, 0.55, 0.55);
    case 2u:
        return vec3(0.45, 0.30, 0.18);
    case 3u:
        return vec3(0.20, 0.65, 0.20);
    case 4u:
        return vec3(0.12, 0.45, 0.12);
    case 5u:
        return vec3(0.05, 0.80, 0.05);
    case 6u:
        return vec3(0.85, 0.20, 0.20);
    case 7u:
        return vec3(0.85, 0.85, 0.85);
    case 9u:
        return vec3(0.15, 0.35, 0.95);
    default:
        return vec3(0.95, 0.95, 0.95);
    }
}

vec4 mappedColor()
{
    if (camera.colorSource == ColorRgb) {
        return color;
    }
    if (camera.colorSource == ColorX) {
        return vec4(scalarColor(float(positionAttributes.x)), 1.0);
    }
    if (camera.colorSource == ColorY) {
        return vec4(scalarColor(float(positionAttributes.y)), 1.0);
    }
    if (camera.colorSource == ColorZ) {
        return vec4(scalarColor(float(positionAttributes.z)), 1.0);
    }
    if (camera.colorSource == ColorIntensity) {
        uint intensity = packedProperties & 0xffffu;
        return vec4(scalarColor(float(intensity)), 1.0);
    }
    if (camera.colorSource == ColorClassification) {
        uint classification = positionAttributes.w & 0xffu;
        return vec4(classificationColor(classification), 1.0);
    }
    if (camera.colorSource == ColorReturnNumber) {
        uint returnNumber = (packedProperties >> 16u) & 0xffu;
        return vec4(scalarColor(float(returnNumber) * 257.0), 1.0);
    }
    if (camera.colorSource == ColorNumberOfReturns) {
        uint numberOfReturns = (packedProperties >> 24u) & 0xffu;
        return vec4(scalarColor(float(numberOfReturns) * 257.0), 1.0);
    }
    return color;
}

void main()
{
    vec3 position = vec3(positionAttributes.xyz) / 65535.0 * 2.0 - 1.0;
    gl_Position = camera.mvp * vec4(position, 1.0);
    gl_PointSize = camera.pointSize;
    pointColor = mappedColor();
}
```

- [ ] **Step 7: Keep the pick shader uniform layout compatible**

Replace the uniform block in `shaders/pick.vert` with:

```glsl
layout(binding = 0) uniform Camera {
    mat4 mvp;
    float pointSize;
    int colorSource;
    float scalarMinimum;
    float scalarMaximum;
} camera;
```

Leave the rest of `pick.vert` unchanged.

- [ ] **Step 8: Build shaders and run renderer/picker tests**

Run:

```bash
cmake --build build --target pcinspector_renderer_contract_tests pcinspector_metal_point_picker_tests
ctest --test-dir build --output-on-failure -R '^(renderer_contract|metal_point_picker)$'
```

Expected: both tests pass. If Qt Shader Tools rejects the shader, fix only the GLSL syntax reported by the tool and rerun the same command.

- [ ] **Step 9: Commit the Metal shader color path**

Run:

```bash
git add src/renderer/metal/MetalRenderViewport.h src/renderer/metal/MetalRenderViewport.cpp shaders/points.vert shaders/pick.vert tests/renderer_contract_tests.cpp
git commit -m "feat: color points in metal shader"
```

---

### Task 4: Add the toolbar color-source selector

**Files:**
- Modify: `src/app/MainWindow.h`
- Modify: `src/app/MainWindow.cpp`
- Test: `tests/main_window_tests.cpp`

- [ ] **Step 1: Write the failing main-window selector test**

Modify `tests/main_window_tests.cpp`.

Add this include:

```cpp
#include <QComboBox>
```

In `ImmediateLoader::load(...)`, after:

```cpp
        cloud->metadata.sourcePointCount = 3;
```

add:

```cpp
        cloud->metadata.hasColor = true;
        cloud->metadata.hasIntensity = true;
        cloud->metadata.hasClassification = true;
```

In `FakeViewport`, add this public accessor:

```cpp
    int colorModeSetCount() const noexcept
    {
        return colorModeSetCount_;
    }
```

In `FakeViewport::setColorMode(...)`, increment the counter before assigning:

```cpp
            ++colorModeSetCount_;
            colorMode_ = mode;
```

Add this private field:

```cpp
    int colorModeSetCount_ = 0;
```

Add this test after `testOpenActionIsSharedByMenuAndToolbar()`:

```cpp
void testColorSourceSelectorUpdatesRenderer()
{
    auto viewport = std::make_unique<FakeViewport>();
    FakeViewport *viewportPointer = viewport.get();
    auto controller = std::make_unique<pci::PointCloudLoadController>(
        std::make_shared<ImmediateLoader>());
    pci::MainWindow window(
        std::move(viewport), std::move(controller), 100);
    window.show();

    auto *combo =
        window.findChild<QComboBox *>(QStringLiteral("colorSourceComboBox"));
    CHECK(combo != nullptr);

    window.loadPointCloud("fixture.las");
    processUntil([&] { return viewportPointer->totalPointCount() == 3; });
    processUntil([&] {
        return combo->findText(QStringLiteral("RGB")) >= 0
               && combo->findText(QStringLiteral("Z")) >= 0
               && combo->findText(QStringLiteral("Intensity")) >= 0
               && combo->findText(QStringLiteral("Classification")) >= 0;
    });

    const int zIndex = combo->findText(QStringLiteral("Z"));
    CHECK(zIndex >= 0);
    const int before = viewportPointer->colorModeSetCount();
    combo->setCurrentIndex(zIndex);
    CHECK(viewportPointer->colorMode().source == pci::PointColorSource::Z);
    CHECK(viewportPointer->colorModeSetCount() == before + 1);
}
```

Call it from `main()` after `testOpenActionIsSharedByMenuAndToolbar();`:

```cpp
        testColorSourceSelectorUpdatesRenderer();
```

- [ ] **Step 2: Run the focused main-window test and verify it fails**

Run:

```bash
cmake --build build --target pcinspector_main_window_tests
ctest --test-dir build --output-on-failure -R '^main_window$'
```

Expected: test fails because `colorSourceComboBox` does not exist.

- [ ] **Step 3: Add main-window members and methods**

Modify `src/app/MainWindow.h`.

Forward-declare `QComboBox` near the existing Qt forward declarations:

```cpp
class QComboBox;
```

Add private methods after `requestLoadCancellation()`:

```cpp
    void refreshColorSourceSelector();
    void applySelectedColorSource(int index);
```

Add this private member near `LoadingOverlay *loadingOverlay_`:

```cpp
    QComboBox *colorSourceComboBox_ = nullptr;
```

- [ ] **Step 4: Create and wire the toolbar combo box**

Modify `src/app/MainWindow.cpp`.

Add includes:

```cpp
#include "renderer/PointColorMode.h"

#include <QComboBox>
#include <QSignalBlocker>
```

After:

```cpp
    pointCloudToolBar->addAction(openAction);
```

add:

```cpp
    colorSourceComboBox_ = new QComboBox(pointCloudToolBar);
    colorSourceComboBox_->setObjectName(
        QStringLiteral("colorSourceComboBox"));
    colorSourceComboBox_->setToolTip(
        QStringLiteral("Point color source"));
    pointCloudToolBar->addWidget(colorSourceComboBox_);
    connect(colorSourceComboBox_,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this](const int index) {
                applySelectedColorSource(index);
            });
    refreshColorSourceSelector();
```

- [ ] **Step 5: Implement selector refresh and application**

Add these methods in `src/app/MainWindow.cpp` after `requestLoadCancellation()`:

```cpp
void MainWindow::refreshColorSourceSelector()
{
    if (!colorSourceComboBox_) {
        return;
    }

    const QSignalBlocker blocker(colorSourceComboBox_);
    colorSourceComboBox_->clear();
    const auto sources = viewport_->availableColorSources();
    for (const PointColorSource source : sources) {
        colorSourceComboBox_->addItem(
            pointColorSourceLabel(source),
            static_cast<int>(source));
    }

    const int currentSource =
        static_cast<int>(viewport_->colorMode().source);
    const int currentIndex =
        colorSourceComboBox_->findData(currentSource);
    if (currentIndex >= 0) {
        colorSourceComboBox_->setCurrentIndex(currentIndex);
    }
    colorSourceComboBox_->setEnabled(
        colorSourceComboBox_->count() > 1);
}

void MainWindow::applySelectedColorSource(const int index)
{
    if (!colorSourceComboBox_ || index < 0) {
        return;
    }

    const bool ok = colorSourceComboBox_->itemData(index).isValid();
    if (!ok) {
        return;
    }

    viewport_->setColorMode({
        .source = static_cast<PointColorSource>(
            colorSourceComboBox_->itemData(index).toInt()),
    });
}
```

- [ ] **Step 6: Refresh the selector after loading a cloud**

Modify `MainWindow::finishLoading(...)` in `src/app/MainWindow.cpp`.

After:

```cpp
        viewport_->setPointCloud(cloud);
```

add:

```cpp
        refreshColorSourceSelector();
```

- [ ] **Step 7: Run the focused main-window test and verify it passes**

Run:

```bash
cmake --build build --target pcinspector_main_window_tests
ctest --test-dir build --output-on-failure -R '^main_window$'
```

Expected: test passes.

- [ ] **Step 8: Commit the UI selector**

Run:

```bash
git add src/app/MainWindow.h src/app/MainWindow.cpp tests/main_window_tests.cpp
git commit -m "feat: add point color source selector"
```

---

### Task 5: Full verification and cleanup

**Files:**
- Inspect: all files changed by Tasks 1 through 4.

- [ ] **Step 1: Run formatting-neutral source checks**

Run:

```bash
rg -n "padding" src tests shaders
rg -n "unfinished marker|temporary marker" src tests shaders
```

Expected: no `padding` references remain in `src`, `tests`, or `shaders`. No unfinished or temporary markers appear in changed files.

- [ ] **Step 2: Build the application and all tests**

Run:

```bash
cmake --build build
```

Expected: build completes successfully, including shader compilation for `points.vert` and `pick.vert`.

- [ ] **Step 3: Run the full automated suite**

Run:

```bash
ctest --test-dir build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 4: Run the renderer smoke tests if a macOS GUI session is available**

Run:

```bash
ctest --test-dir build --output-on-failure -R '^renderer_smoke'
```

Expected: renderer smoke tests pass. If this command fails because the execution environment has no WindowServer access, rerun it from a normal logged-in macOS GUI session before merging.

- [ ] **Step 5: Review changed files**

Run:

```bash
git status --short
git diff --stat HEAD
git diff HEAD -- src/core/GpuPoint.h src/core/GpuPointProperties.h src/import/pdal/PdalPointCloudLoader.cpp src/renderer/PointColorMode.h src/renderer/RenderViewport.h src/renderer/metal/MetalRenderViewport.cpp shaders/points.vert shaders/pick.vert src/app/MainWindow.cpp
```

Expected: only color-map-related files are changed, and the diff shows no point-buffer rebuild on color-mode change.

- [ ] **Step 6: Commit verification-only fixes if any were needed**

If Step 1 through Step 5 required source fixes, commit those fixes:

```bash
git add src tests shaders
git commit -m "fix: stabilize point color map implementation"
```

If no fixes were needed, do not create an empty commit.
