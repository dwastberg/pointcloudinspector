# Point-cloud thinning: implementation findings

**Date:** 2026-07-18
**Scope:** Selection and implementation guidance for three point-cloud thinning algorithms:

1. **HAVSampler** — highest-throughput option.
2. **Feature fusion with FW-VFPS** — general-purpose quality option.
3. **TFRSUB** — terrain-specialized option.

The requested output ratio is treated as a target with a permitted error of **±10 percentage points**. The design below can nevertheless produce an exact point count for the general-purpose method and a close or exact count for the other methods when needed.

---

## 1. Executive recommendation

| Profile | Recommended algorithm | Primary advantage | Main cost |
|---|---|---|---|
| Speed first | HAVSampler-style hierarchical adaptive voxel sampling | Linear or near-linear, highly parallel, streamable | Limited awareness of fine geometric features |
| General use | Feature fusion + feature-weighted voxel FPS (FW-VFPS) | Good balance of feature retention, coverage, and controllable output size | Neighborhood and feature computation |
| Terrain | TFRSUB, or the implementable TFRSUB-inspired specification in this document | Optimizes terrain fidelity and spatial uniformity | Highest complexity; no public reference implementation was located |

### Practical default

For a production application that must support arbitrary large files, implement a common tiled infrastructure and expose three modes:

```text
fast       -> hierarchical adaptive voxel selection
balanced   -> feature fusion + voxel-local weighted FPS
terrain    -> terrain features + clustered residual/uncertainty selection
```

The methods should return **indices into the original cloud**, not reconstructed points. This preserves all LAS/COPC attributes and allows the writer to copy the complete original point record.

---

## 2. Requirements and assumptions

### 2.1 Inputs

At minimum:

```text
x, y, z
```

Common optional dimensions that must be preserved:

```text
intensity
return number
number of returns
classification
scan angle
GPS time
RGB/NIR
point source ID
user-defined extra dimensions
```

Terrain mode assumes either:

- the input contains only ground points; or
- ground points can be selected using the LAS classification dimension.

### 2.2 Output-count contract

Let:

- `N` be the number of eligible input points;
- `r` be the requested retention fraction;
- `K = round(r * N)` be the nominal target;
- `t = 0.10` be the tolerance in fraction units for ±10 percentage points.

The accepted output interval is:

```text
K_min = ceil(max(0, r - t) * N)
K_max = floor(min(1, r + t) * N)
```

This interval is very broad for small targets. At a target of 5%, an absolute ±10-point tolerance permits anything from 0% to 15%. A safer application-level policy is:

```text
absolute tolerance = min(10 percentage points,
                         max(2 percentage points, 0.25 * target percentage))
```

Alternatively, always compute `K` and let algorithms hit it exactly when the additional cost is low.

### 2.3 Non-goals

These algorithms are for selecting a subset of existing points. They do not:

- fit or generate a new surface;
- merge points into centroids unless explicitly configured;
- replace terrain breakline extraction;
- guarantee preservation of every thin structure without semantic or geometric hints.

---

## 3. Shared application architecture

All three algorithms benefit from the same infrastructure.

```text
PointCloudReader
    |
    v
EligibilityFilter
    |
    v
SpatialPartitioner -----> GlobalBudgetAllocator
    |
    v
TileProcessor
    |- neighborhood search
    |- feature computation
    |- local selector
    |
    v
GlobalReconciler
    |
    v
PointCloudWriter
```

### 3.1 Recommended components

#### Reader/writer

Use PDAL or a native LAS/COPC library for:

- LAS/LAZ/COPC reading;
- spatially bounded reads;
- classification filtering;
- preservation of arbitrary dimensions;
- COPC output.

COPC is particularly suitable because it stores LAZ in a clustered octree. Aligning the thinning tile hierarchy with COPC nodes reduces unnecessary reads and makes progressive processing easier.

#### Spatial indexing

Choose based on the working-set size:

| Scale | Suggested structure |
|---|---|
| Prototype or small tile | `scipy.spatial.cKDTree`, PCL k-d tree, or nanoflann |
| Dense organized scans | image-grid or range-image neighborhoods |
| General large tiles | voxel hash plus local neighbor-cell search |
| Repeated multi-scale use | Morton-sorted octree |
| GPU | sorted voxel keys plus segmented operations |

A voxel hash is often faster than a k-d tree when only approximate local neighborhoods are required.

#### Parallel execution

Use one of:

- OpenMP;
- oneTBB;
- a thread pool with work-stealing;
- CUDA/HIP for voxelization, reductions, and local selection.

Parallelize over tiles and, within a tile, over points during feature calculation.

### 3.2 Two-pass out-of-core design

A robust large-cloud implementation uses two passes.

#### Pass 1: planning

Collect:

- eligible point count;
- global bounds;
- per-tile counts;
- optional occupancy counts at several voxel levels;
- optional low-cost complexity statistics.

Use these statistics to select voxel sizes and allocate quotas.

#### Pass 2: selection

For each tile:

1. read the core and halo;
2. compute required features;
3. select only points owned by the core;
4. write selected global point IDs or directly stream selected point records;
5. discard the tile working set.

This avoids holding the entire cloud in memory.

### 3.3 Core tiles and halos

Neighborhood features near tile boundaries require points outside the tile.

```text
read region  = core bounds expanded by halo
emit region  = core bounds only
```

Set the halo to at least the largest neighborhood radius. Every point must have exactly one owner tile, defined using half-open bounds or a global integer tile key.

### 3.4 Determinism

For deterministic output:

- use a global coordinate origin for all voxel keys;
- use fixed-point or carefully defined floor operations for coordinates;
- resolve equal scores using the original 64-bit point index;
- avoid hash iteration order as a tie-breaker;
- use a fixed seed for any randomized initialization;
- combine parallel reductions in a stable order when bitwise repeatability matters.

### 3.5 Common API

A possible C++ interface:

```cpp
enum class ThinningMethod {
    FastHierarchicalVoxel,
    FeatureFusionFWVFPS,
    TerrainTFRSUB
};

struct ThinningOptions {
    ThinningMethod method;
    double target_ratio;          // [0, 1]
    double tolerance_pp = 10.0;   // absolute percentage points

    double tile_size_xy = 250.0;  // coordinate units
    double halo = 2.0;
    std::uint64_t random_seed = 0;

    bool exact_count = false;
    bool preserve_boundaries = true;
    bool preserve_extrema = true;
    bool use_ground_only = false;
    std::uint8_t ground_class = 2;

    std::size_t knn_small = 16;
    std::size_t knn_large = 48;
};

struct ThinningStatistics {
    std::uint64_t input_points;
    std::uint64_t eligible_points;
    std::uint64_t selected_points;
    double achieved_ratio;
    double elapsed_seconds;
    std::uint64_t peak_memory_bytes;
};

struct ThinningResult {
    std::vector<std::uint64_t> selected_indices;
    ThinningStatistics statistics;
};
```

For very large outputs, replace the vector with a streamed bitmap, sorted index file, or callback.

---

# Part I — speed-first model

## 4. HAVSampler-style hierarchical adaptive voxel sampling

### 4.1 Source method

HAVSampler was introduced in:

> Ouyang, J., Liu, X., and Chen, H. *Hierarchical Adaptive Voxel-guided Sampling for Real-time Applications in Large-scale Point Clouds*. arXiv:2305.14306, 2023.

The paper presents a hierarchical adaptive voxel-guided sampler with linear complexity and high parallelism. Its experiments report more than a 100× speed-up over farthest-point sampling in the evaluated scene-scale neural-network pipelines.

The available public repository is an integrated C++/CUDA/TensorRT point-cloud detector rather than a standalone thinning library. It is useful for implementation ideas, but a production thinning application will likely need an independent implementation.

### 4.2 Core idea

Construct a hierarchy of aligned voxels. At a selected level, emit one representative point per occupied voxel. Adaptively refine selected voxels when a finer output is required.

Let base voxel size be `h0`. Level `l` uses:

```text
h_l = h0 * 2^l
```

For point `p = (x, y, z)` and global origin `o`:

```text
ix = floor((x - ox) / h_l)
iy = floor((y - oy) / h_l)
iz = floor((z - oz) / h_l)
```

The tuple `(l, ix, iy, iz)` is encoded as a Morton key or a hash key.

### 4.3 Representative choices

In increasing order of cost:

1. **First point in deterministic input/index order**
2. **Point nearest voxel center**
3. **Point nearest voxel centroid**
4. **Point with the highest supplied importance**
5. **Point minimizing local reconstruction error**

For the speed mode, use the point nearest the voxel center. It avoids producing synthetic centroids and provides a stable spatial distribution.

### 4.4 Target-ratio selection

A single voxel size controls the point count only indirectly. The hierarchy provides a more robust way to land inside the requested interval.

#### Method A: select the nearest complete level

During the planning pass, count occupied voxels at several levels. Choose the level whose occupancy is closest to `K`.

This is the cheapest option and is often enough for ±10 percentage points.

#### Method B: mixed-level adaptive refinement

Use a coarse level whose occupied-voxel count is below the target. Each occupied parent voxel can then be replaced by its occupied child voxels.

Refining parent `v` changes the count by:

```text
delta(v) = occupied_children(v) - 1
```

Refine parents until the count enters `[K_min, K_max]`, or until the next refinement would overshoot badly.

Possible refinement priorities:

```text
speed-only:       largest delta first
uniform:          breadth-first or round-robin by region
feature-biased:   highest local residual/variation first
terrain-biased:   highest terrain-complexity score first
```

This retains hierarchical speed while allowing much finer count control than selecting one complete octree level.

#### Method C: exact reconciliation

When exact output is requested:

- if too many points, remove representatives from the least important refined cells;
- if too few, add a second representative from selected cells, chosen as the point farthest from the existing representative.

This extra step is optional under the ±10-point contract.

### 4.5 CPU implementation

A sort-based implementation is deterministic and memory-efficient:

```text
for each point:
    compute voxel key
    append (key, point_index, distance_to_voxel_center)

radix_sort by (key, distance, point_index)

for each key run:
    output first item
```

Complexity:

```text
voxel-key calculation: O(N)
radix sort:            O(N) for fixed-width integer keys in practice
selection:             O(N)
memory:                O(N) for a tile or external-sort run
```

A hash-based implementation has expected `O(N)` time but requires explicit deterministic tie handling.

### 4.6 GPU implementation

A common CUDA/HIP pipeline:

1. calculate voxel keys and squared center distances;
2. radix-sort pairs by key;
3. segmented minimum by `(distance, point ID)`;
4. compact winning indices;
5. optionally construct parent keys by bit shifts and repeat.

Libraries such as CUB or rocPRIM provide radix sort and segmented primitives.

### 4.7 Out-of-core handling

Use a globally aligned voxel origin. Do not independently shift the origin in each tile, because that creates seams.

Two viable strategies:

- spatially partition the input so every voxel belongs to one tile;
- emit `(voxel_key, candidate)` records into external-sort runs and reduce globally.

The first is simpler if tiles align with coarse voxels.

### 4.8 Pseudocode

```python
def hav_thin(points, target_ratio, tolerance_pp, base_size):
    target = round(target_ratio * len(points))
    lower = ceil(max(0.0, target_ratio - tolerance_pp / 100) * len(points))
    upper = floor(min(1.0, target_ratio + tolerance_pp / 100) * len(points))

    hierarchy = build_occupied_voxel_hierarchy(points, base_size)

    level = coarsest_level_with_count_at_most(hierarchy, target)
    selected_nodes = occupied_nodes(hierarchy, level)

    candidates = priority_queue(
        selected_nodes,
        key=lambda node: refinement_priority(node)
    )

    count = len(selected_nodes)

    while count < lower and candidates:
        node = candidates.pop()
        children = occupied_children(node)
        if len(children) <= 1:
            continue

        selected_nodes.remove(node)
        selected_nodes.extend(children)
        count += len(children) - 1

        for child in children:
            candidates.push(child)

        if lower <= count <= upper:
            break

    return [
        point_nearest_voxel_center(node.points, node.bounds)
        for node in selected_nodes
    ]
```

### 4.9 Strengths

- fastest of the three selected methods;
- works well with spatial hierarchies and COPC;
- readily parallelized;
- outputs original points;
- deterministic;
- naturally supports progressive levels of detail.

### 4.10 Weaknesses

- voxel size, rather than surface error, drives simplification;
- thin structures may vanish;
- a representative near a voxel center is not necessarily geometrically important;
- raw XYZ voxels may be inappropriate for highly anisotropic data;
- terrain fidelity can be poor in ridges, ditches, and breaklines.

### 4.11 Suggested initial parameters

```yaml
base_voxel_size: estimate from median nearest-neighbor spacing
level_factor: 2
representative: nearest_voxel_center
hierarchy_dimensions:
  general_3d: xyz
  terrain_fast_variant: xy
exact_count: false
```

Estimate `h0` so the finest hierarchy is at or below the original sampling scale.

---

# Part II — general-purpose model

## 5. Feature fusion with FW-VFPS

### 5.1 Source method

The selected general method is:

> Chao, J., Lei, J., Zhou, X., and Xie, L. *A general and flexible point cloud simplification method based on feature fusion*. Displays 88, 103007, 2025. DOI: 10.1016/j.displa.2025.103007.

The authors provide a public C++ implementation. The reference implementation is modular and includes:

- preprocessing/downsampling;
- feature analysis;
- voxel division;
- voxel grouping;
- regional point-budget allocation;
- feature-weighted voxel farthest-point sampling;
- final point-number adjustment.

The repository lists PCL, CGAL, VTK, and fmt as dependencies. The core thinning design does not inherently require CGAL; a new implementation can use PCL or Eigen plus a neighborhood library. Review the repository and third-party licenses before incorporating code.

### 5.2 Why it is the recommended general default

A global top-score selector tends to concentrate points at edges. Pure FPS gives good coverage but ignores geometric importance. FW-VFPS combines:

- regional quotas;
- spatially local farthest-point selection;
- feature weights;
- constraints from already processed neighboring voxels.

This gives a useful balance between feature preservation and uniformity.

### 5.3 Pipeline

```text
optional cheap pre-thinning
        |
        v
neighborhood construction
        |
        v
multi-feature calculation
        |
        v
robust normalization and fusion
        |
        v
voxel partitioning
        |
        v
quota allocation
        |
        v
feature-weighted voxel FPS
        |
        v
global count reconciliation
```

### 5.4 Neighborhood selection

Start with two scales:

```text
small neighborhood: k = 16
large neighborhood: k = 48
```

Use the small scale for edges and fine detail, and the large scale for stable normals and broad surface shape.

For varying density, radius neighborhoods may be preferable, but they make runtime and memory less predictable. A hybrid option is:

```text
radius = c * local_spacing
clamp neighbor count to [k_min, k_max]
```

### 5.5 Feature definitions

Let `N_i` be the neighborhood of point `p_i`, with centroid `c_i`. Let covariance eigenvalues satisfy:

```text
lambda_0 <= lambda_1 <= lambda_2
```

and let `n_i` be the eigenvector of `lambda_0`.

#### Squared spacing

```text
spacing_i = mean(||p_j - p_i||^2 for j in N_i)
```

This compensates for input-density variation.

#### Surface variation

```text
surface_variation_i =
    lambda_0 / (lambda_0 + lambda_1 + lambda_2 + epsilon)
```

#### Normal difference

Use the absolute dot product so inconsistent normal signs do not create false edges:

```text
normal_difference_i =
    mean(1 - abs(dot(n_i, n_j)) for j in N_i)
```

#### Variation difference

```text
variation_difference_i =
    mean(abs(surface_variation_i - surface_variation_j)
         for j in N_i)
```

#### Projection residual

```text
projection_residual_i =
    mean(abs(dot(n_i, p_j - p_i)) for j in N_i)
```

This measures deviation from the local tangent plane.

#### Optional curvature features

A local quadratic or principal-curvature fit can provide:

```text
mean_curvature_i = (abs(k1) + abs(k2)) / 2
gaussian_feature_i = sqrt(abs(k1 * k2))
```

These are more expensive and can be postponed until the basic implementation is working.

### 5.6 Robust normalization

Do not normalize each feature by only its global maximum. A single outlier can flatten all other scores.

Recommended robust scaling:

```text
q_low  = percentile(feature, 5)
q_high = percentile(feature, 95)

normalized =
    clamp((feature - q_low) / max(q_high - q_low, epsilon), 0, 1)
```

For tiled processing, estimate global percentiles from a planning sample so all tiles use comparable scales.

### 5.7 Feature fusion

The public implementation supports weighted fusion of several features. A practical score is:

```text
score_i =
    w_s  * normalized_spacing_i +
    w_sv * normalized_surface_variation_i +
    w_nd * normalized_normal_difference_i +
    w_vd * normalized_variation_difference_i +
    w_pr * normalized_projection_residual_i
```

Suggested starting weights, supplied as engineering defaults rather than paper parameters:

```yaml
spacing: 0.10
surface_variation: 0.20
normal_difference: 0.25
variation_difference: 0.15
projection_residual: 0.30
```

For clean manufactured surfaces, reduce spacing and increase normal/projection terms. For noisy LiDAR, reduce curvature-sensitive terms and use robust neighborhoods.

### 5.8 Voxel partitioning

Choose a voxel size large enough that a voxel normally contains several output points, not merely one. A useful starting estimate is:

```text
expected input points per voxel =
    4 to 16 times expected output points per voxel
```

The voxel is a computational region, not necessarily the final sample spacing.

### 5.9 Quota allocation

Let voxel `v` contain points `P_v`. Define its raw weight:

```text
W_v = sum(epsilon + score_i for i in P_v)
```

A basic ideal quota is:

```text
q_v = K * W_v / sum(W_u)
```

However, feature-only allocation can starve flat regions. Use a mixture of population and feature complexity:

```text
population_share_v = |P_v| / N
feature_share_v = W_v / sum(W_u)

q_v = K * (
    alpha * population_share_v +
    (1 - alpha) * feature_share_v
)
```

Recommended initial value:

```text
alpha = 0.4
```

Apply:

```text
0 <= quota_v <= |P_v|
```

For exact `K`, use largest-remainder allocation:

1. assign `floor(q_v)` to every voxel;
2. distribute remaining points by descending fractional remainder;
3. skip saturated voxels.

### 5.10 Feature-weighted voxel FPS

Within one voxel, the algorithm maintains the distance from every candidate to its nearest selected or constraint point.

A useful feature-weighted objective is:

```text
priority_i =
    min_distance_squared_i * (epsilon + score_i)^gamma
```

Start with:

```text
gamma = 1
```

Selection:

1. initialize with the point farthest from the voxel center, or farthest from neighbor constraints;
2. update each candidate's nearest-selected distance;
3. select the candidate with maximum weighted priority;
4. repeat until the voxel quota is met.

### 5.11 Cross-voxel constraints

Independent FPS in every voxel can form clusters or gaps along voxel boundaries.

Process voxels in eight parity groups:

```text
group = (ix mod 2, iy mod 2, iz mod 2)
```

When selecting a voxel, include already selected points from adjacent voxels as fixed constraint points. This encourages a continuous distribution across boundaries.

Alternative:

- process voxel graph coloring in parallel;
- use a halo of already selected boundary points;
- run a final local repulsion pass around voxel seams.

### 5.12 Count reconciliation

The reference implementation includes explicit number fine-tuning. A general implementation can store the marginal selection priority associated with every accepted point and the best rejected candidate in each voxel.

If too few points:

```text
repeatedly add the best rejected candidate globally
```

If too many:

```text
repeatedly remove the accepted point with the lowest marginal value,
excluding mandatory points and the first coverage point of nonempty regions
```

This can produce exactly `K`. Because exact count is cheap once candidates and scores exist, it is recommended even though the application permits ±10 percentage points.

### 5.13 Pseudocode

```python
def feature_fusion_thin(points, K, options):
    neighbors_small = build_neighbors(points, k=options.k_small)
    neighbors_large = build_neighbors(points, k=options.k_large)

    normals, eigenvalues = estimate_normals_and_eigenvalues(
        points, neighbors_large
    )

    features = {
        "spacing": squared_spacing(points, neighbors_small),
        "surface_variation": surface_variation(eigenvalues),
        "normal_difference": normal_difference(
            normals, neighbors_small
        ),
        "variation_difference": variation_difference(
            eigenvalues, neighbors_small
        ),
        "projection_residual": projection_residual(
            points, normals, neighbors_small
        ),
    }

    normalized = robust_normalize(features)
    scores = weighted_sum(normalized, options.feature_weights)

    voxels = partition_into_voxels(points, options.voxel_size)
    quotas = allocate_exact_quotas(voxels, scores, K)

    selected = []
    rejected_frontiers = []

    for color_group in voxel_graph_colors(voxels):
        parallel_for voxel in color_group:
            constraints = selected_points_in_neighbor_voxels(voxel)
            local_selected, frontier = weighted_fps(
                points_in(voxel),
                scores,
                quotas[voxel],
                constraints
            )
            selected.extend(local_selected)
            rejected_frontiers.append(frontier)

    selected = reconcile_to_exact_count(
        selected,
        rejected_frontiers,
        K
    )

    return selected
```

### 5.14 Scaling modifications for very large clouds

The reference workflow is appropriate for in-memory clouds. For very large data:

1. compute global planning statistics;
2. allocate exact quotas to spatial tiles using largest remainder;
3. process tile plus halo;
4. compute local features;
5. run voxel selection only in the core;
6. merge selected point IDs;
7. perform a small global correction if required.

Use approximate voxel neighborhoods rather than a global k-d tree.

A low-cost pre-stage may reduce a very dense cloud to 2–4 times the final requested count before feature calculation. Do not pre-thin so aggressively that important structures disappear before scoring.

### 5.15 Strengths

- general-purpose and training-free;
- keeps original points;
- count can be controlled exactly;
- feature set is configurable;
- handles density and geometry jointly;
- regional selection avoids global edge overconcentration;
- public reference code is available.

### 5.16 Weaknesses

- neighborhood computation is substantially slower than voxel thinning;
- feature parameters are scale-dependent;
- normal and curvature estimates are sensitive to noise and mixed surfaces;
- per-voxel FPS can be expensive in highly populated voxels;
- the public code is not a drop-in cross-platform library.

---

# Part III — terrain model

## 6. TFRSUB and an implementable terrain specification

### 6.1 Source method

The selected terrain method is:

> Chen, C., Yang, Z., Pan, H., Li, Y., and Hao, J. *TFRSUB: A terrain-feature retention and spatial uniformity balancing method for simplifying LiDAR ground point clouds*. ISPRS Journal of Photogrammetry and Remote Sensing 232, 389–407, 2026. DOI: 10.1016/j.isprsjprs.2025.12.015.

The published summary describes three main elements:

1. **Distance-Geometric Synergy Index (DGSI)** combining orthogonal deviation distance and sampling interval, intended to reduce boundary contraction.
2. **Composite Terrain Factor (CTF)** combining several terrain parameters.
3. **Cluster-driven Gaussian Process Regression (GPR)** using CTF for iterative feature-point selection.

The evaluation covers eight terrain datasets, six retention ratios, and airborne, terrestrial, and UAV LiDAR. Reported comparisons include DEM elevation, slope, and mean-curvature errors.

### 6.2 Reproducibility status

No public TFRSUB code repository was located as of 2026-07-18. The publicly accessible abstract and metadata do not expose all equations, weights, or update details needed for a faithful reproduction.

Therefore, distinguish between:

- **faithful TFRSUB reproduction**, which requires access to the full paper and careful validation against its experiments; and
- **TFRSUB-inspired implementation**, specified below and suitable for starting application development immediately.

Do not label the proposed equations below as the authors' exact equations.

### 6.3 Terrain-specific assumptions

Use 2.5D geometry:

```text
z = f(x, y)
```

Neighborhood searches, spatial spacing, and suppression distances should normally use XY distance. Z participates in plane/quadratic fitting and feature calculation but should not distort spatial density.

Before thinning:

- retain only classified ground points;
- remove isolated outliers;
- split overhangs or multi-valued XY regions if they remain;
- preserve user-provided breaklines as mandatory points.

### 6.4 Local terrain model

Fit a robust local plane or quadratic surface in centered coordinates:

```text
z =
    a*x^2 + b*x*y + c*y^2 +
    d*x + e*y + f
```

Use weighted least squares or iteratively reweighted least squares. Center and scale local XY coordinates before fitting to improve conditioning.

Derive:

```text
dz/dx = 2*a*x + b*y + d
dz/dy = b*x + 2*c*y + e
```

At the center:

```text
slope_magnitude = sqrt(d^2 + e^2)
```

The local Hessian is:

```text
H = [[2*a, b],
     [b, 2*c]]
```

Its eigenvalues provide simple curvature indicators.

### 6.5 Candidate terrain features

#### Orthogonal/local-fit residual

For a local plane:

```text
residual_i = abs(dot(n_i, p_i - c_i))
```

For the quadratic model:

```text
residual_i = abs(z_i - z_hat_i)
```

Use robust fitting so a real breakline is not erased as an outlier.

#### XY sampling interval

```text
spacing_i =
    mean(||xy_i - xy_j|| for j in XY-neighbors)
```

#### Slope-change score

```text
slope_change_i =
    mean(||gradient_i - gradient_j|| for j in neighbors)
```

#### Curvature score

A simple scale-independent starting score:

```text
curvature_i =
    sqrt(kappa_1^2 + kappa_2^2)
```

where `kappa_1`, `kappa_2` are derived from the fitted local surface or a stable approximation.

#### Roughness

```text
roughness_i =
    median(abs(residual_j - median(residual_neighbors)))
```

or use the MAD of Z residuals around the local model.

#### Local extrema strength

```text
extrema_i =
    abs(z_i - median(z_j for j in neighbors))
```

Flag a point as a strict local maximum or minimum only after a noise threshold is exceeded.

#### Boundary score

CGAL-free options include:

- point lies in a partially occupied border cell;
- large angular gap between sorted XY-neighbor bearings;
- local alpha-shape approximation using a 2D Delaunay library with acceptable licensing;
- distance to a supplied polygon or breakline.

The angular-gap approach:

1. find XY neighbors;
2. calculate bearings around the point;
3. sort bearings;
4. find maximum circular gap;
5. classify as a boundary candidate when the gap exceeds a threshold.

### 6.6 Practical DGSI surrogate

The paper states that DGSI combines orthogonal geometric deviation and sampling interval. Until the exact full-paper definition is implemented, use this explicitly labeled engineering surrogate:

```text
DGSI_star_i =
    alpha * robust_scale(residual_i) +
    (1 - alpha) * robust_scale(spacing_i)
```

Starting value:

```text
alpha = 0.7
```

Use `DGSI_star` only to rank candidates or trigger mandatory boundary retention, not as a claim of reproducing the published equation.

### 6.7 Practical composite terrain factor

A proposed CTF-like score:

```text
CTF_star_i =
    w_r  * residual_i_norm +
    w_sc * slope_change_i_norm +
    w_c  * curvature_i_norm +
    w_e  * extrema_i_norm +
    w_ro * roughness_i_norm +
    w_sp * spacing_i_norm
```

Engineering starting weights:

```yaml
local_fit_residual: 0.30
slope_change: 0.20
curvature: 0.20
extrema: 0.15
roughness: 0.10
spacing: 0.05
```

For flood modeling and drainage analysis, increase curvature/extrema and add explicit ridge/valley or flow-accumulation features. For contour production, prioritize residual and slope change.

### 6.8 Mandatory seed set

Before iterative selection, reserve:

- user-supplied breakline points;
- robust local elevation minima and maxima;
- boundary candidates above a high DGSI-like threshold;
- one coverage seed per nonempty coarse XY cell;
- optional points at tile/catchment boundaries.

If mandatory seeds exceed the target:

1. never discard explicit user constraints;
2. rank inferred extrema and boundaries;
3. retain the highest-confidence inferred points;
4. report that the target cannot be satisfied without violating constraints.

### 6.9 Spatial clustering

The paper uses a cluster-driven GPR strategy. For large clouds, cluster candidates before fitting any GP.

Options:

- mini-batch k-means on normalized `(x, y)`;
- fixed XY tiles followed by connected-component grouping;
- watershed/catchment regions if hydrological structure is known;
- recursive quadtree until each cluster has at most `M` candidates.

A practical cluster size for an exact dense GP is small, often hundreds rather than tens of thousands. Standard dense GP fitting scales cubically in the training-set size, so use:

- many small clusters;
- sparse/inducing-point GP;
- a local kernel model;
- or the residual-greedy substitute in Section 6.12.

### 6.10 Cluster budget allocation

Reserve `K_seed` mandatory points. Allocate the remaining budget:

```text
K_remaining = K - K_seed
```

For cluster `c`:

```text
population_share_c = N_c / sum(N)
complexity_share_c = sum(CTF_star_i in c) / sum(CTF_star_i)

quota_c =
    K_remaining *
    (beta * population_share_c +
     (1 - beta) * complexity_share_c)
```

Start with:

```text
beta = 0.4
```

Use largest-remainder allocation for exact global quotas.

### 6.11 GPR-driven iterative selection

A practical interpretation is to model terrain elevation:

```text
z = f(x, y)
```

inside each cluster. Fit the GP to the currently selected points and predict mean and standard deviation for remaining candidates.

A proposed acquisition function is:

```text
acquisition_i =
    sigma_i *
    (epsilon + CTF_star_i)^gamma *
    coverage_i
```

where:

```text
sigma_i = GP posterior standard deviation
coverage_i =
    min(1, distance_xy_to_selected_i / desired_spacing)
```

Start with:

```text
gamma = 1
```

At each iteration:

1. fit or update the local GP;
2. compute acquisition for remaining candidates;
3. select the maximum;
4. add it to the training set;
5. repeat until the cluster quota is reached.

For numerical stability:

- center and scale XY;
- normalize Z;
- include a noise term;
- constrain kernel length scale;
- avoid refitting hyperparameters at every iteration;
- use batched additions rather than selecting one point per complete refit.

A reasonable prototype kernel is:

```text
Constant * Matern(nu=1.5) + WhiteNoise
```

A Matérn kernel is often more realistic than an infinitely smooth RBF for terrain.

### 6.12 Scalable residual-greedy substitute

For large production data, a GP may be too expensive. A strong substitute is:

1. build a coarse TIN, raster, or local quadratic model from seeds;
2. calculate vertical residual for all candidates;
3. multiply residual by the terrain factor and a spacing term;
4. add the highest-scoring batch;
5. update only affected local regions;
6. repeat until the quota is filled.

Proposed priority:

```text
priority_i =
    abs(z_i - z_model_i) *
    (epsilon + CTF_star_i)^gamma *
    coverage_i
```

This follows the same terrain-fidelity principle while being easier to scale and debug.

### 6.13 Tile implementation

For each XY tile:

1. read ground points in core + halo;
2. calculate multi-scale neighborhoods in XY;
3. fit terrain features;
4. identify mandatory seeds owned by the core;
5. allocate or receive a core quota;
6. cluster core candidates, allowing halo points as context;
7. select points;
8. emit only core-owned points.

Use overlap reconciliation for inferred boundary/extrema flags, but never emit halo-owned duplicates.

### 6.14 Target-ratio handling

TFRSUB-like thresholding may naturally produce an approximate count. The application should prefer quota-driven stopping because a target ratio is explicitly supplied.

Under ±10 percentage points:

- exact quotas are not required;
- stopping each cluster within a small local tolerance will normally satisfy the global interval;
- nevertheless, exact largest-remainder quotas make behavior predictable and cost little.

### 6.15 Pseudocode

```python
def terrain_thin(ground_points, K, options):
    xy_neighbors_small = build_xy_neighbors(
        ground_points, k=options.k_small
    )
    xy_neighbors_large = build_xy_neighbors(
        ground_points, k=options.k_large
    )

    local_models = fit_robust_quadratic_surfaces(
        ground_points,
        xy_neighbors_large
    )

    features = compute_terrain_features(
        ground_points,
        xy_neighbors_small,
        xy_neighbors_large,
        local_models
    )

    normalized = robust_normalize(features)

    dgsi_star = (
        0.7 * normalized["residual"] +
        0.3 * normalized["spacing"]
    )

    ctf_star = weighted_sum(
        normalized,
        options.terrain_feature_weights
    )

    mandatory = select_mandatory_seeds(
        ground_points,
        dgsi_star,
        normalized["extrema"],
        options.breakline_indices
    )

    clusters = spatial_clusters(
        ground_points,
        max_candidates=options.max_cluster_candidates
    )

    quotas = allocate_cluster_quotas(
        clusters,
        ctf_star,
        K - len(mandatory)
    )

    selected = set(mandatory)

    for cluster in clusters:
        local_selected = selected.intersection(cluster)

        while number_new_points(local_selected) < quotas[cluster]:
            model = fit_local_terrain_model(local_selected)

            mean, uncertainty = model.predict(
                remaining_candidates(cluster)
            )

            acquisition = (
                uncertainty *
                (epsilon + ctf_star) *
                coverage_to_selected(cluster, local_selected)
            )

            next_batch = arg_top_k(acquisition, batch_size)
            local_selected.update(next_batch)

        selected.update(local_selected)

    return reconcile_to_exact_count(selected, K)
```

### 6.16 Strengths

- terrain-specific rather than generic 3D geometry;
- explicitly balances feature retention and spatial uniformity;
- suitable metrics align with DEM use;
- preserves ridges, valleys, boundaries, and local extrema more deliberately;
- quota-driven form supports requested retention ratios.

### 6.17 Weaknesses

- most complex implementation;
- faithful reproduction requires the full paper;
- no public code was located;
- GP fitting must be localized or approximated;
- terrain-feature weights and neighborhood scales require validation;
- depends on reliable ground classification.

---

## 7. Choosing between the three methods at runtime

### 7.1 Decision rules

```text
Is processing speed or interactive LOD generation dominant?
    yes -> HAVSampler-style

Does the input contain general 3D geometry and quality matters?
    yes -> feature fusion + FW-VFPS

Is the input a ground surface used for DEM/terrain analysis?
    yes -> TFRSUB or the terrain implementation described here
```

### 7.2 Suggested user-facing profiles

```yaml
fast:
  algorithm: hierarchical_voxel
  representative: nearest_voxel_center
  adaptive_refinement: true
  exact_count: false

balanced:
  algorithm: feature_fusion_fw_vfps
  features:
    - spacing
    - surface_variation
    - normal_difference
    - projection_residual
  exact_count: true

terrain:
  algorithm: terrain_ctf_clustered
  dimensions: xy_plus_z_model
  preserve_boundaries: true
  preserve_extrema: true
  selector: residual_greedy  # switch to GPR for maximum quality
  exact_count: true
```

---

## 8. Recommended implementation plan

### Milestone 1: common I/O and index-preserving pipeline

Deliver:

- LAS/LAZ/COPC read and write;
- eligibility/classification filter;
- tile/core/halo ownership;
- 64-bit original point IDs;
- selected-index writer;
- throughput and memory statistics.

Acceptance tests:

- all selected records exactly preserve original dimensions;
- no duplicates or dropped selected IDs at tile seams;
- deterministic results across thread counts where configured.

### Milestone 2: fast hierarchical voxel mode

Deliver:

- voxel-key generation;
- representative selection;
- occupancy counts at multiple levels;
- complete-level target selection;
- adaptive parent refinement;
- optional count reconciliation.

Acceptance tests:

- linear throughput trend;
- output within target interval;
- no tile-origin seams;
- stable output under input permutation when deterministic mode is enabled.

### Milestone 3: balanced feature-fusion mode

Deliver:

- kNN or voxel neighborhoods;
- PCA normals/eigenvalues;
- spacing, surface variation, normal difference, and projection residual;
- robust normalization;
- exact tile/voxel quotas;
- weighted FPS;
- boundary constraints and exact count reconciliation.

Acceptance tests:

- exact `K`;
- better point-to-plane and normal error than fast mode at equal `K`;
- no strong grid or tile-boundary artifacts.

### Milestone 4: terrain baseline

Before GPR, implement:

- XY neighborhoods;
- robust local plane/quadratic fits;
- terrain score;
- mandatory boundary/extrema points;
- feature-weighted spatial selection;
- exact cluster quotas.

This baseline provides most of the infrastructure required by a TFRSUB-inspired method.

### Milestone 5: clustered terrain residual or GPR refinement

Deliver:

- cluster construction;
- local terrain model;
- uncertainty or residual acquisition;
- batch iterative updates;
- DEM-specific evaluation.

Compare dense GP, sparse GP, and residual-greedy variants before choosing the production default.

---

## 9. Evaluation protocol

Evaluate every method at identical target ratios, for example:

```text
5%, 10%, 20%, 40%, 60%, 80%
```

Record both nominal and achieved ratios.

### 9.1 Performance

- elapsed time;
- CPU time;
- peak resident memory;
- read/write time separately;
- points processed per second;
- parallel scaling;
- temporary disk usage;
- GPU transfer and kernel time where applicable.

### 9.2 General geometric quality

#### Symmetric nearest-neighbor distance

```text
D(P, S) =
    mean_{p in P} min_{s in S} ||p-s|| +
    mean_{s in S} min_{p in P} ||s-p||
```

Because `S` is a subset of `P`, the second term is normally zero, but retaining the symmetric metric is useful when comparing with methods that generate new points.

#### Point-to-plane error

For each original point, find the nearest selected point or reconstructed local surface and measure displacement along the local normal.

#### Normal deviation

Compare normals estimated on the original and thinned clouds or on surfaces reconstructed from them.

#### Coverage

Measure:

- mean and 95th percentile nearest-selected distance;
- maximum hole radius;
- voxel occupancy uniformity;
- coefficient of variation of local output density.

#### Feature retention

Evaluate separately on:

- high-curvature regions;
- detected edges;
- thin structures;
- boundaries.

### 9.3 Terrain quality

Generate a reference and thinned DEM using the same interpolation settings.

Measure:

- elevation MAE and RMSE;
- 95th and maximum absolute elevation error;
- slope MAE/RMSE;
- aspect error;
- profile and plan-curvature error;
- contour displacement;
- ridge and valley displacement;
- breakline omission;
- drainage direction agreement;
- flow accumulation/catchment changes;
- volume differences for cut/fill use cases.

Do not evaluate terrain quality only with 3D Chamfer distance.

### 9.4 Target-count quality

```text
count_error_pp =
    100 * abs(selected_count / eligible_count - target_ratio)
```

Also report:

- count error before final reconciliation;
- count error after reconciliation;
- correction-stage runtime.

### 9.5 Dataset coverage

Include:

- uniform synthetic planes;
- curved synthetic surfaces with known ground truth;
- sharp crease and thin-feature synthetic clouds;
- airborne terrain;
- terrestrial terrain;
- UAV terrain;
- flat farmland;
- urban ground with curbs/ditches;
- steep natural terrain;
- varying-density and overlapping flight strips;
- clouds with realistic noise and outliers.

---

## 10. Important implementation pitfalls

### 10.1 Confusing absolute and relative tolerance

“±10 percentage points” is not the same as “±10% of the target.” Document the API clearly.

### 10.2 Voxel grids with tile-local origins

This produces visible seams and inconsistent counts. Always use a global origin.

### 10.3 Losing LAS attributes

Select original indices and copy complete point records. Do not reconstruct output records from only XYZ.

### 10.4 Globally selecting the largest feature scores

This overpopulates edges and can leave flat regions uncovered. Allocate regional quotas and enforce coverage.

### 10.5 Normal sign errors

Use `abs(dot(n_i, n_j))` unless normals have been consistently oriented.

### 10.6 Fixed neighborhood scales across varying density

Use multiple scales or local-spacing-adjusted radii.

### 10.7 GPR on an entire terrain cloud

Dense GP fitting is not viable at large `N`. Cluster, use inducing points, or replace it with local residual-greedy refinement.

### 10.8 Evaluating terrain only with point distances

DEM derivatives and hydrological structure are more meaningful than generic 3D distances.

### 10.9 Overaggressive candidate pre-thinning

A coarse voxel pre-pass can permanently remove breaklines before the feature-aware algorithm sees them. Use a conservative pre-pass or retain multiple candidates per voxel.

### 10.10 Ignoring mandatory-point budget

Explicit constraints may already exceed the requested count. Define and report this behavior instead of silently dropping constraints.

---

## 11. Reference implementation notes

### HAVSampler repository

Repository:

- https://github.com/OuyangJunyuan/pointcloud-3d-detector-tensorrt

Use it as a source of C++/CUDA integration ideas. It is primarily a TensorRT detector implementation and is not packaged as a generic point-cloud thinning library.

### Feature-fusion repository

Repository:

- https://github.com/chaojiale/PointCloudSimplification

Relevant modules visible in the repository include:

```text
DownSampling.hpp
FeatureAnalysis.hpp
NumberFineTuning.hpp
ProcessFlow.hpp
Simplification.hpp
VoxelDivision.hpp
VoxelFarthestPointSampling.hpp
```

The high-level process in `ProcessFlow.hpp` closely matches:

```text
preprocess
normalize
feature analysis
voxel division
active-neighbor construction
voxel grouping
target allocation
simplification
point-number fine tuning
```

The code is valuable for validating formulas and control flow, but a new cross-platform application should separate the core library from visualization and test programs.

### TFRSUB

Paper:

- https://doi.org/10.1016/j.isprsjprs.2025.12.015

No public repository was located. Obtain the full paper before claiming an exact reproduction.

---

## 12. Suggested technology stack

### Production C++ implementation

```text
Language:      C++20
I/O:           PDAL or LASzip-compatible library
Math:          Eigen
Neighborhoods: PCL, nanoflann, or custom voxel hash
Parallelism:   oneTBB or OpenMP
GPU optional:  CUDA/CUB or HIP/rocPRIM
Clustering:    custom mini-batch k-means or suitable library
GP prototype:  separate Python validation or a sparse GP library
Build:         CMake
Tests:         Catch2 or GoogleTest
```

A CGAL-free design is feasible for all core algorithms described here.

### Python research prototype

```text
numpy
scipy.spatial.cKDTree
laspy and/or PDAL Python bindings
scikit-learn MiniBatchKMeans
scikit-learn GaussianProcessRegressor for small clusters
rasterio/GDAL for DEM evaluation
```

Use Python for validating features, weights, and evaluation metrics. Move voxelization, neighborhoods, and weighted FPS to C++/Rust/CUDA for large production workloads.

### Possible Rust core

A Rust implementation can use:

```text
rayon for CPU parallelism
kiddo or a custom spatial index
morton encoding crates or a custom encoder
las/laz crates or PDAL FFI for I/O
nalgebra for local covariance/eigendecomposition
```

The most reusable boundary is a library that consumes point batches and returns selected original indices.

---

## 13. Minimal first prototypes

### 13.1 Fast prototype

Implement in this order:

```text
1. fixed XYZ voxel sampling
2. nearest-center representative
3. target voxel-size search
4. multi-level occupancy
5. adaptive parent refinement
6. tile/out-of-core support
```

### 13.2 General prototype

```text
1. kNN neighborhoods
2. PCA normals and surface variation
3. projection residual
4. robust feature fusion
5. voxel quota allocation
6. weighted FPS
7. neighbor-voxel constraints
8. exact count reconciliation
```

### 13.3 Terrain prototype

```text
1. ground-only XY tiling
2. local robust plane fit
3. residual + spacing + extrema score
4. mandatory extrema/boundary seeds
5. exact regional quotas
6. feature-weighted Poisson/FPS selection
7. local quadratic terrain features
8. residual-greedy iterative refinement
9. clustered GPR experiment
```

The terrain prototype should initially use residual-greedy refinement. Add GPR only after the feature calculations, quotas, and evaluation suite are stable.

---

## 14. Final selection summary

### Highest speed

**HAVSampler-style hierarchical adaptive voxel sampling**

Use when:

- throughput dominates;
- the cloud is extremely large;
- the output is for visualization, LOD, or neural-network preprocessing;
- ±10 percentage points is acceptable.

### Best general approach

**Feature fusion with FW-VFPS**

Use when:

- the input contains arbitrary 3D surfaces;
- feature preservation and coverage both matter;
- more computation is acceptable;
- predictable or exact output size is useful.

This is the recommended default mode for a general-purpose thinning application.

### Best terrain approach

**TFRSUB, implemented faithfully from the full paper or approximated using the explicit terrain pipeline in this document**

Use when:

- the points represent ground terrain;
- DEM, slope, curvature, ridges, valleys, and drainage fidelity matter;
- computation can be spent on terrain features and iterative selection.

For very large production terrain, begin with the residual-greedy TFRSUB-inspired variant and evaluate whether clustered GPR provides enough quality improvement to justify its cost.

---

# References

## Selected algorithms

1. Ouyang, J., Liu, X., and Chen, H. **Hierarchical Adaptive Voxel-guided Sampling for Real-time Applications in Large-scale Point Clouds.** arXiv:2305.14306, 2023.
   Paper: https://arxiv.org/abs/2305.14306
   Code/integrated implementation: https://github.com/OuyangJunyuan/pointcloud-3d-detector-tensorrt

2. Chao, J., Lei, J., Zhou, X., and Xie, L. **A general and flexible point cloud simplification method based on feature fusion.** Displays, 88, 103007, 2025.
   DOI: https://doi.org/10.1016/j.displa.2025.103007
   Code: https://github.com/chaojiale/PointCloudSimplification

3. Chen, C., Yang, Z., Pan, H., Li, Y., and Hao, J. **TFRSUB: A terrain-feature retention and spatial uniformity balancing method for simplifying LiDAR ground point clouds.** ISPRS Journal of Photogrammetry and Remote Sensing, 232, 389–407, 2026.
   DOI: https://doi.org/10.1016/j.isprsjprs.2025.12.015

## Implementation resources

4. PDAL documentation.
   https://pdal.io/en/stable/

5. PDAL COPC writer documentation.
   https://pdal.io/en/stable/stages/writers.copc.html

6. COPC specification and resources.
   https://copc.io/

7. Point Cloud Library: normal estimation.
   https://pointclouds.org/documentation/tutorials/normal_estimation.html

8. Point Cloud Library: feature-estimation overview.
   https://pointclouds.org/documentation/tutorials/how_features_work.html

9. SciPy `cKDTree`.
   https://docs.scipy.org/doc/scipy/reference/generated/scipy.spatial.cKDTree.html

10. scikit-learn Gaussian-process documentation.
    https://scikit-learn.org/stable/modules/gaussian_process.html

11. scikit-learn `MiniBatchKMeans`.
    https://scikit-learn.org/stable/modules/generated/sklearn.cluster.MiniBatchKMeans.html

## Foundational geometric reference

12. Pauly, M., Gross, M., and Kobbelt, L. P. **Efficient Simplification of Point-Sampled Surfaces.** IEEE Visualization, 2002.
    This work is a foundational reference for covariance-derived surface variation and feature-preserving point simplification.

---

## Verification notes

- Web sources and public repositories were checked on **2026-07-18**.
- Performance figures quoted for research algorithms are author-reported and should be re-benchmarked on the application's own datasets and hardware.
- The TFRSUB-inspired formulas and default weights in this document are engineering proposals, not a claim of reproducing unpublished or inaccessible details from the TFRSUB paper.
- Review the licenses of all repositories and dependencies before copying source code into a commercial or redistributable application.
