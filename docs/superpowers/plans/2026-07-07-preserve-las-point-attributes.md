# Preserve LAS Point Attributes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve common LAS/LAZ/COPC per-point attributes during import so intensity, classification, return number, and number of returns can be used by coloring and filtering features.

**Architecture:** Keep `GpuPoint` as the compact render payload and add a separate CPU-side `PointAttributes` stream in `LoadedPointCloud`. The PDAL loader builds sortable point records, sorts render points and attributes together, then publishes aligned vectors where `points[i]` and `attributes[i]` describe the same source point.

**Tech Stack:** C++23, PDAL 2.10, Qt/CMake test targets, existing no-framework C++ test executables.

---

## File Structure

- Create `src/pointcloud/PointAttributes.h`: small POD record for attributes preserved per loaded point.
- Modify `src/pointcloud/LoadedPointCloud.h`: add `std::vector<PointAttributes> attributes`.
- Modify `src/pointcloud/SourcePoint.h`: keep source-side defaults aligned with the preserved common attributes.
- Modify `src/pointcloud/PointCloudMetadata.h`: add metadata flags for return fields.
- Modify `src/import/pdal/PdalSourceInspector.cpp`: detect `ReturnNumber` and `NumberOfReturns`.
- Modify `src/import/pdal/PdalPointCloudLoader.cpp`: read full attribute values and keep them aligned through sorting.
- Modify `tests/fixtures/PdalFixtureFactory.{h,cpp}`: write deterministic return attributes into fixture LAS/LAZ/COPC files.
- Modify `tests/pointcloud_contract_tests.cpp`: verify the CPU data contract.
- Modify `tests/pdal_import_tests.cpp`: verify imported metadata and per-point attributes.
- Modify `tests/renderer_contract_tests.cpp`: verify renderer-facing clouds can carry the new attribute vector without changing rendering behavior.

---

### Task 1: Add the CPU attribute data contract

**Files:**
- Create: `src/pointcloud/PointAttributes.h`
- Modify: `src/pointcloud/LoadedPointCloud.h`
- Modify: `src/pointcloud/SourcePoint.h`
- Test: `tests/pointcloud_contract_tests.cpp`

- [ ] **Step 1: Write the failing point-cloud contract tests**

Modify `tests/pointcloud_contract_tests.cpp`.

Add this include:

```cpp
#include "pointcloud/PointAttributes.h"
```

In `testLoadedCloudContractIsImmutableByConvention()`, after `mutableCloud->points.push_back({});`, add:

```cpp
    mutableCloud->attributes.push_back({
        .intensity = 1234,
        .classification = 5,
        .returnNumber = 2,
        .numberOfReturns = 3,
    });
```

After `CHECK(cloud->points.size() == 1);`, add:

```cpp
    CHECK(cloud->attributes.size() == cloud->points.size());
    CHECK(cloud->attributes.front().intensity == 1234);
    CHECK(cloud->attributes.front().classification == 5);
    CHECK(cloud->attributes.front().returnNumber == 2);
    CHECK(cloud->attributes.front().numberOfReturns == 3);
```

Add this test function after `testLoadedCloudContractIsImmutableByConvention()`:

```cpp
void testPointAttributesDefaultsAreNeutral()
{
    const pci::PointAttributes attributes;
    CHECK(attributes.intensity == 0);
    CHECK(attributes.classification == 0);
    CHECK(attributes.returnNumber == 0);
    CHECK(attributes.numberOfReturns == 0);
    static_assert(std::is_trivially_copyable_v<pci::PointAttributes>);
    static_assert(sizeof(pci::PointAttributes) <= 8);
}
```

Extend `testSourcePointDefaultsAreRenderable()`:

```cpp
    CHECK(point.returnNumber == 0);
    CHECK(point.numberOfReturns == 0);
```

Call the new test from `main()` after `testLoadedCloudContractIsImmutableByConvention();`:

```cpp
        testPointAttributesDefaultsAreNeutral();
```

- [ ] **Step 2: Run the focused contract test and verify it fails**

Run:

```bash
cmake --build build --target pcinspector_pointcloud_contract_tests
ctest --test-dir build --output-on-failure -R '^pointcloud_contract$'
```

Expected: build fails because `pointcloud/PointAttributes.h`, `LoadedPointCloud::attributes`, `SourcePoint::returnNumber`, and `SourcePoint::numberOfReturns` do not exist.

- [ ] **Step 3: Add the point attribute type**

Create `src/pointcloud/PointAttributes.h`:

```cpp
#pragma once

#include <cstdint>

namespace pci {

struct PointAttributes {
    std::uint16_t intensity = 0;
    std::uint8_t classification = 0;
    std::uint8_t returnNumber = 0;
    std::uint8_t numberOfReturns = 0;

    bool operator==(const PointAttributes &) const = default;
};

static_assert(sizeof(PointAttributes) <= 8);

} // namespace pci
```

- [ ] **Step 4: Store attributes beside render points**

Modify `src/pointcloud/LoadedPointCloud.h`.

Add this include:

```cpp
#include "pointcloud/PointAttributes.h"
```

Add the vector immediately after `std::vector<GpuPoint> points;`:

```cpp
    std::vector<PointAttributes> attributes;
```

- [ ] **Step 5: Extend source point defaults**

Modify `src/pointcloud/SourcePoint.h`:

```cpp
struct SourcePoint {
    std::array<double, 3> position{};
    std::uint32_t rgba = 0xffffffffU;
    std::uint16_t intensity = 0;
    std::uint8_t classification = 0;
    std::uint8_t returnNumber = 0;
    std::uint8_t numberOfReturns = 0;
    std::uint64_t sourceOrdinal = 0;
};
```

- [ ] **Step 6: Run the focused contract test and verify it passes**

Run:

```bash
cmake --build build --target pcinspector_pointcloud_contract_tests
ctest --test-dir build --output-on-failure -R '^pointcloud_contract$'
```

Expected: test passes and prints `point-cloud contract tests passed`.

- [ ] **Step 7: Commit the data contract**

Run:

```bash
git add src/pointcloud/PointAttributes.h src/pointcloud/LoadedPointCloud.h src/pointcloud/SourcePoint.h tests/pointcloud_contract_tests.cpp
git commit -m "feat: add point attribute data contract"
```

---

### Task 2: Detect and generate return attributes in PDAL fixtures

**Files:**
- Modify: `src/pointcloud/PointCloudMetadata.h`
- Modify: `src/import/pdal/PdalSourceInspector.cpp`
- Modify: `tests/fixtures/PdalFixtureFactory.h`
- Modify: `tests/fixtures/PdalFixtureFactory.cpp`
- Test: `tests/pdal_import_tests.cpp`

- [ ] **Step 1: Write failing metadata assertions**

Modify `tests/pdal_import_tests.cpp`.

In `checkMetadata()`, after `CHECK(metadata.hasClassification);`, add:

```cpp
    CHECK(metadata.hasReturnNumber);
    CHECK(metadata.hasNumberOfReturns);
```

- [ ] **Step 2: Run the focused PDAL import test and verify it fails**

Run:

```bash
cmake --build build --target pcinspector_pdal_import_tests
ctest --test-dir build --output-on-failure -R '^pdal_import$'
```

Expected: build fails because `PointCloudMetadata::hasReturnNumber` and `PointCloudMetadata::hasNumberOfReturns` do not exist.

- [ ] **Step 3: Add metadata flags**

Modify `src/pointcloud/PointCloudMetadata.h`.

After `bool hasClassification = false;`, add:

```cpp
    bool hasReturnNumber = false;
    bool hasNumberOfReturns = false;
```

- [ ] **Step 4: Extend the deterministic fixture point schema**

Modify `tests/fixtures/PdalFixtureFactory.h`.

Replace `FixturePoint` with:

```cpp
struct FixturePoint {
    double x;
    double y;
    double z;
    std::uint16_t red;
    std::uint16_t green;
    std::uint16_t blue;
    std::uint16_t intensity;
    std::uint8_t classification;
    std::uint8_t returnNumber;
    std::uint8_t numberOfReturns;
};
```

Replace `fixturePoints` with:

```cpp
inline constexpr std::array<FixturePoint, 8> fixturePoints{{
    {1000.0, 2000.0, 10.0, 65535,     0,     0, 100, 2, 1, 2},
    {1010.0, 2000.0, 10.0,     0, 65535,     0, 200, 2, 2, 2},
    {1000.0, 2010.0, 10.0,     0,     0, 65535, 300, 5, 1, 1},
    {1010.0, 2010.0, 10.0, 65535, 65535,     0, 400, 5, 1, 3},
    {1000.0, 2000.0, 20.0, 65535,     0, 65535, 500, 6, 2, 3},
    {1010.0, 2000.0, 20.0,     0, 65535, 65535, 600, 6, 3, 3},
    {1000.0, 2010.0, 20.0, 32768, 32768, 32768, 700, 1, 1, 4},
    {1010.0, 2010.0, 20.0, 65535, 65535, 65535, 800, 1, 4, 4},
}};
```

- [ ] **Step 5: Write return attributes into fixture files**

Modify `tests/fixtures/PdalFixtureFactory.cpp`.

In `layout->registerDims({ ... })`, add:

```cpp
        pdal::Dimension::Id::ReturnNumber,
        pdal::Dimension::Id::NumberOfReturns,
```

After setting `Classification`, add:

```cpp
        view->setField(
            pdal::Dimension::Id::ReturnNumber,
            index,
            point.returnNumber);
        view->setField(
            pdal::Dimension::Id::NumberOfReturns,
            index,
            point.numberOfReturns);
```

- [ ] **Step 6: Detect return dimensions in the inspector**

Modify `src/import/pdal/PdalSourceInspector.cpp`.

In the `PointCloudMetadata metadata{...}` initializer, after `hasClassification`, add:

```cpp
            .hasReturnNumber =
                containsDimension(info.m_dimNames, "ReturnNumber"),
            .hasNumberOfReturns =
                containsDimension(info.m_dimNames, "NumberOfReturns"),
```

- [ ] **Step 7: Run metadata/fixture tests and verify they pass**

Run:

```bash
cmake --build build --target pcinspector_pdal_fixture_tests pcinspector_pdal_import_tests
ctest --test-dir build --output-on-failure -R '^(pdal_fixture|pdal_import)$'
```

Expected: both tests pass.

- [ ] **Step 8: Commit metadata and fixtures**

Run:

```bash
git add src/pointcloud/PointCloudMetadata.h src/import/pdal/PdalSourceInspector.cpp tests/fixtures/PdalFixtureFactory.h tests/fixtures/PdalFixtureFactory.cpp tests/pdal_import_tests.cpp
git commit -m "feat: detect las return attributes"
```

---

### Task 3: Preserve imported attributes through PDAL loading and sorting

**Files:**
- Modify: `src/import/pdal/PdalPointCloudLoader.cpp`
- Test: `tests/pdal_import_tests.cpp`

- [ ] **Step 1: Write failing import assertions for preserved attributes**

Modify `tests/pdal_import_tests.cpp`.

In `testLoadsEquivalentPointsFromAllFormats()`, after `CHECK(las->points == copc->points);`, add:

```cpp
    CHECK(las->attributes == laz->attributes);
    CHECK(las->attributes == copc->attributes);
```

After `CHECK(las->points.size() == pci::test::fixturePoints.size());`, add:

```cpp
    CHECK(las->attributes.size() == las->points.size());
```

After `CHECK((las->points.front().attributes & 0xffU) == 2);`, add:

```cpp
    CHECK(las->attributes.front().intensity == 100);
    CHECK(las->attributes.front().classification == 2);
    CHECK(las->attributes.front().returnNumber == 1);
    CHECK(las->attributes.front().numberOfReturns == 2);
```

In `testPointLimitUsesDeterministicStride()`, after `CHECK(first->points == repeated->points);`, add:

```cpp
    CHECK(first->attributes == repeated->attributes);
    CHECK(first->attributes.size() == first->points.size());
```

After `CHECK(first->points[2].rgba == 0xffff0000U);`, add:

```cpp
    CHECK(first->attributes[0].intensity == 100);
    CHECK(first->attributes[0].classification == 2);
    CHECK(first->attributes[0].returnNumber == 1);
    CHECK(first->attributes[0].numberOfReturns == 2);
    CHECK(first->attributes[1].intensity == 500);
    CHECK(first->attributes[1].classification == 6);
    CHECK(first->attributes[1].returnNumber == 2);
    CHECK(first->attributes[1].numberOfReturns == 3);
```

- [ ] **Step 2: Run the focused PDAL import test and verify it fails**

Run:

```bash
cmake --build build --target pcinspector_pdal_import_tests
ctest --test-dir build --output-on-failure -R '^pdal_import$'
```

Expected: test fails because the loader does not populate `LoadedPointCloud::attributes`.

- [ ] **Step 3: Include the attribute header in the loader**

Modify `src/import/pdal/PdalPointCloudLoader.cpp`.

Add:

```cpp
#include "pointcloud/PointAttributes.h"
```

- [ ] **Step 4: Add a sortable import record**

In the anonymous namespace of `src/import/pdal/PdalPointCloudLoader.cpp`, before `quantizeCoordinate()`, add:

```cpp
struct LoadedPointRecord {
    GpuPoint point;
    PointAttributes attributes;
};
```

- [ ] **Step 5: Add full attribute mapping**

In the anonymous namespace, after `color8()`, add:

```cpp
PointAttributes mapAttributes(const pdal::PointRef &point,
                              const PointCloudMetadata &metadata)
{
    return PointAttributes{
        .intensity = metadata.hasIntensity
            ? point.getFieldAs<std::uint16_t>(
                  pdal::Dimension::Id::Intensity)
            : std::uint16_t{0},
        .classification = metadata.hasClassification
            ? point.getFieldAs<std::uint8_t>(
                  pdal::Dimension::Id::Classification)
            : std::uint8_t{0},
        .returnNumber = metadata.hasReturnNumber
            ? point.getFieldAs<std::uint8_t>(
                  pdal::Dimension::Id::ReturnNumber)
            : std::uint8_t{0},
        .numberOfReturns = metadata.hasNumberOfReturns
            ? point.getFieldAs<std::uint8_t>(
                  pdal::Dimension::Id::NumberOfReturns)
            : std::uint8_t{0},
    };
}
```

- [ ] **Step 6: Build records instead of sorting render points alone**

In `PdalPointCloudLoader::load()`, replace the initial reserve block:

```cpp
    cloud->points.reserve(static_cast<std::size_t>(std::min(
        metadata.sourcePointCount, request.maximumPoints)));
```

with:

```cpp
    const std::size_t reservedPointCount = static_cast<std::size_t>(
        std::min(metadata.sourcePointCount, request.maximumPoints));
    std::vector<LoadedPointRecord> records;
    records.reserve(reservedPointCount);
```

In the stream callback, replace:

```cpp
            if (processed % stride == 0
                && cloud->points.size() < request.maximumPoints) {
                cloud->points.push_back(
                    mapPoint(point, metadata, center, extent));
            }
```

with:

```cpp
            if (processed % stride == 0
                && records.size() < request.maximumPoints) {
                records.push_back({
                    .point = mapPoint(point, metadata, center, extent),
                    .attributes = mapAttributes(point, metadata),
                });
            }
```

Replace the optimizing progress block:

```cpp
            .processed = cloud->points.size(),
            .total = cloud->points.size(),
```

with:

```cpp
            .processed = records.size(),
            .total = records.size(),
```

Replace:

```cpp
    std::sort(cloud->points.begin(), cloud->points.end(), pointLess);
```

with:

```cpp
    std::sort(
        records.begin(),
        records.end(),
        [](const LoadedPointRecord &left, const LoadedPointRecord &right) {
            return pointLess(left.point, right.point);
        });

    cloud->points.reserve(records.size());
    cloud->attributes.reserve(records.size());
    for (const LoadedPointRecord &record : records) {
        cloud->points.push_back(record.point);
        cloud->attributes.push_back(record.attributes);
    }
```

- [ ] **Step 7: Run the focused PDAL import test and verify it passes**

Run:

```bash
cmake --build build --target pcinspector_pdal_import_tests
ctest --test-dir build --output-on-failure -R '^pdal_import$'
```

Expected: test passes and prints `PDAL import tests passed`.

- [ ] **Step 8: Commit attribute preservation**

Run:

```bash
git add src/import/pdal/PdalPointCloudLoader.cpp tests/pdal_import_tests.cpp
git commit -m "feat: preserve imported point attributes"
```

---

### Task 4: Verify renderer compatibility and whole-project behavior

**Files:**
- Modify: `tests/renderer_contract_tests.cpp`

- [ ] **Step 1: Write a renderer compatibility assertion**

Modify `tests/renderer_contract_tests.cpp`.

Find this block:

```cpp
        auto loaded = std::make_shared<pci::LoadedPointCloud>();
        loaded->metadata.sourcePointCount = 2;
        loaded->points.resize(2);
```

Replace it with:

```cpp
        auto loaded = std::make_shared<pci::LoadedPointCloud>();
        loaded->metadata.sourcePointCount = 2;
        loaded->points.resize(2);
        loaded->attributes.push_back({
            .intensity = 1024,
            .classification = 2,
            .returnNumber = 1,
            .numberOfReturns = 1,
        });
        loaded->attributes.push_back({
            .intensity = 2048,
            .classification = 5,
            .returnNumber = 2,
            .numberOfReturns = 2,
        });
        CHECK(loaded->attributes.size() == loaded->points.size());
```

- [ ] **Step 2: Run the renderer contract test**

Run:

```bash
cmake --build build --target pcinspector_renderer_contract_tests
ctest --test-dir build --output-on-failure -R '^renderer_contract$'
```

Expected: renderer contract test passes. The renderer continues using `LoadedPointCloud::points`; attribute storage is passive in this phase.

- [ ] **Step 3: Run focused non-GUI verification**

Run:

```bash
cmake --build build
ctest --test-dir build --output-on-failure -R '^(pointcloud_contract|pdal_fixture|pdal_import|renderer_contract)$'
```

Expected: all listed tests pass.

- [ ] **Step 4: Run full test suite**

Run:

```bash
ctest --test-dir build --output-on-failure
```

Expected: all tests pass. On macOS, if WindowServer access is required for Metal/Qt tests, rerun this command with GUI execution approval.

- [ ] **Step 5: Commit renderer compatibility coverage**

Run:

```bash
git add tests/renderer_contract_tests.cpp
git commit -m "test: cover point attributes in renderer clouds"
```

---

## Self-Review

- Spec coverage: the plan preserves intensity, classification, return number, and number of returns in the loaded CPU point cloud; metadata reports availability; tests cover fixture generation, import, sorting alignment, point limiting, and renderer compatibility.
- Scope boundary: this plan does not add UI controls, shader color modes, GPU attribute buffers, or filtering. Those features should consume `LoadedPointCloud::attributes` in separate rendering/UI plans.
- Type consistency: the same `PointAttributes` fields are used in tests, fixtures, loader mapping, and `LoadedPointCloud`.
- Sorting correctness: the loader sorts `LoadedPointRecord` values, not `GpuPoint` values alone, so attributes remain aligned with render points.
