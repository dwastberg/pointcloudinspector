# Running CI

## Manual GitHub checks

CI runs only when started manually. In GitHub's **Actions** tab, choose **CI
Linux**, **CI Windows**, **CI macOS**, **CI Clang-Tidy**, **CI ASan/UBSan**, or
**CI TSan**, then select **Run workflow**, choose a branch, and start the run.
Enable **Include full-scale stress tests** when needed; it defaults to off.

Each workflow runs independently, so you can start checks in any order or run
different checks concurrently. Starting the same check again on the same branch
cancels its earlier run. Pushes and pull requests do not start CI automatically.
The package workflows also remain separate manual actions.

New workflow files must be present on the repository's default branch before
GitHub exposes their manual trigger. Once available, the branch selector lets
you run them against another branch. See [GitHub's manual workflow
instructions](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow).

## Linux CI on a Mac

From the repository root, run:

```sh
./scripts/test-linux-container.py
```

To run natively on Apple silicon, without Rosetta:

```sh
./scripts/test-linux-container.py --arch arm64
```

The default remains `--arch amd64`, matching CI's x86-64 architecture through
Rosetta. ARM and x86-64 use separate dependency images and report their selected
architecture and translation mode in the console and JSON summary.

Requires Apple silicon, Python 3, Git, and **Apple container 1.5.0 or newer**.
Initialize the test dependency first if this is a new checkout:

```sh
git submodule update --init --recursive
```

The launcher starts the container service if necessary, builds an Ubuntu 24.04
dependency image for the selected architecture, and runs `ci`, `clang-tidy`, `asan-ubsan`, and `tsan`
sequentially. Each configuration gets a fresh container with 4 CPUs and 16 GiB
memory. The dependency image is reused; source and build directories are fresh
on every run. Current working-tree changes and nonignored new files are included,
including changes inside initialized submodules. No source changes are made by
the launcher.

The image uses the same two micromamba environment manifests as GitHub Actions.
For ARM, the validation manifest is derived by changing only the compiler package
from `clangxx_linux-64` to `clangxx_linux-aarch64`, retaining its version constraint
and all other dependencies. The checked-in CI manifests are unchanged. Matching
native micromamba and Linux packages are downloaded for each architecture.
Both environments are installed in one image, so the first run downloads several
gigabytes. Linux Actions and the containers invoke `run-linux-ci.sh`, preserving
the presets, build parallelism, test parallelism, sanitizer settings, formatting
check, and test timeouts. Job limits are 60 minutes for `ci` and 90 minutes for
each validation configuration. Tests run as an unprivileged Linux user with a
`0022` umask (so cache parent directories are not group-writable), on the
guest filesystem, not on a macOS source mount.

## Results

The final table reports each configuration's status and CTest counts. Failures
do not prevent later configurations from running. Any failure, interruption,
missing results, or incomplete suite makes the command exit nonzero.

Reports are retained in `build/container-ci/<run-id>/`:

- `summary.json`: status, failed stage, duration, test counts and failed names,
  source commit and working-tree status, snapshot checksum, and runtime versions.
- `image.json`: image identity and configuration.
- `image-context/*.yml`: the effective dependency manifests used for the image.
- `image-build.log`: image build output, when an image was built.
- `<preset>/`: container output, configure/build/test logs, CTest diagnostics,
  CTest and Catch2 JUnit reports, and the resolved dependency list.
- `input/source.tar`: the source snapshot used for that run.

Temporary test containers are removed even after failure or interruption. The
image, shared image builder, and service remain available. Delete old report
directories when no longer needed; they contain full source snapshots.

## Options

```sh
./scripts/test-linux-container.py --preset tsan
./scripts/test-linux-container.py --preset ci --preset clang-tidy
./scripts/test-linux-container.py --long-stress
./scripts/test-linux-container.py --cpus 6 --memory 24G
./scripts/test-linux-container.py --rebuild-image
./scripts/test-linux-container.py --dns 192.168.1.1
```

Long stress tests are excluded by default, matching ordinary CI runs.
`--rebuild-image` refreshes the base image and reinstalls dependencies. Changes to
the image recipe or dependency manifests select a new cached image automatically.
The manifests contain version ranges rather than complete locks: a new solve may
differ from an earlier CI run. Consult the retained package lists when comparing.

The default guest and builder DNS resolver is `1.1.1.1`, because the default
container resolver failed on the macOS 15 validation host. Override it with
`--dns` when your network requires another resolver. This does not change host or
system-wide container settings. macOS 15 uses the default container network;
the launcher does not require custom networks.

## Interpretation

The supported baseline is Apple container 1.5.0. The launcher requires matching
CLI and running service versions, and records them in every report. If an update
leaves an older service running, restart it before testing.

With `--arch amd64`, Rosetta executes the x86-64 tools under an ARM Linux kernel. This matches CI's
Linux user-space dependencies more closely than a native ARM build, but does not
reproduce GitHub's native x86-64 host. For TSan the launcher passes
`--kernel-arg sysctl.vm.mmap_rnd_bits=28` and verifies that value before building.
Sanitizer startup failures remain failures; there is no ARM fallback or relaxed
test timeout. The TSan preset applies the dependency exclusions described below.
Inspect the logs before attributing a failure to application code or translation.

Each configuration first checks CMake child-process handling with a 30-second
limit (plus 5 seconds to terminate). This detects a Rosetta hang reproduced on
the macOS 15.7.1 validation host: CMake sleeps indefinitely with a pending
`SIGCHLD` and a defunct child. Updating the container CLI to 1.5.0 alone did not
resolve it. The [upstream libuv report](https://github.com/libuv/libuv/issues/4279)
reports the reproduction passing on macOS 26.2. If this check fails, results show
an environment failure and retain `process-check.log`; application tests have
not run. A passing probe reduces startup ambiguity but cannot rule out later
translation issues.

`--arch arm64` runs the same presets and process check natively, omitting Rosetta
from the container invocation. This avoids the translation layer and exercises
Linux behavior, but uses ARM packages and does not reproduce CI's x86-64
architecture. Failures remain failures on both architectures; the launcher never
automatically switches architecture or relaxes dependency versions.

Only the four Linux CI configurations are covered. Windows, macOS, packaging,
and native GPU jobs require their own environments.

## TSan dependency exclusions

The `tsan` test preset loads `cmake/tsan.supp` in both CI and local containers.
It retains every test and assertion, and fails on unsuppressed sanitizer reports.
Three exclusions cover false positives reproduced with standalone programs against
conda-forge PDAL 2.10.2, PROJ 9.9.0, and Qt 6.12.0 using Clang TSan 22.1.8 on Linux ARM64:

- `pdal::las::pdrfDims`: TSan misses the synchronization protecting first-use
  initialization of the prebuilt library's constant LAS dimension table.
- `osgeo::proj::operation::areEquivalentParameters`: concurrent WKT parsing
  with separate PROJ contexts triggers the same issue for a constant set of
  equivalent parameter names. This surfaced during repeated multifile testing.
- Intercepted accesses originating in `libQt6Core.so`: TSan misses Qt's internal
  event-queue synchronization. A function-only exclusion does not cover reports
  whose incomplete stacks show only Qt's array allocator.

The QtCore exclusion reduces detection inside that library. Application code
remains instrumented; a deliberate race in a standalone instrumented program
still fails with these exclusions enabled. Revisit the exclusions when upgrading
dependencies or building them with TSan instrumentation. Investigate new reports
separately before extending this file.

## Launcher regression tests

The launcher regression tests use only Python's standard library:

```sh
python3 -m unittest discover -s scripts/tests -v
```
