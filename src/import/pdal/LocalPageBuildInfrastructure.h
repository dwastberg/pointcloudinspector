#pragma once

#include "import/local/LocalPointIndexBuilder.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace pci::local_index {

class LocalPageBuildLock final {
public:
    explicit LocalPageBuildLock(std::filesystem::path path);
    ~LocalPageBuildLock();

    LocalPageBuildLock(const LocalPageBuildLock &) = delete;
    LocalPageBuildLock &operator=(const LocalPageBuildLock &) = delete;

    [[nodiscard]] bool tryAcquire();
    void touch() const;
    [[nodiscard]] bool removeIfStale(std::chrono::minutes maximumAge) const;

private:
    std::filesystem::path path_;
    bool owned_ = false;
};

class LocalPageBuildSession final {
public:
    LocalPageBuildSession(const std::filesystem::path &cacheDirectory,
                          const std::string &key);
    ~LocalPageBuildSession();

    LocalPageBuildSession(const LocalPageBuildSession &) = delete;
    LocalPageBuildSession &operator=(const LocalPageBuildSession &) = delete;

    [[nodiscard]] const std::filesystem::path &directory() const noexcept;

private:
    std::filesystem::path directory_;
};

class LocalPageCacheLocator final {
public:
    LocalPageCacheLocator(const PointCloudImportPreflight &preflight,
                          const LocalPointPageStoreOptions &options);

    [[nodiscard]] const LocalPointSourceFingerprint &
    fingerprint() const noexcept;
    [[nodiscard]] const std::string &key() const noexcept;
    [[nodiscard]] const std::filesystem::path &finalDirectory() const noexcept;
    [[nodiscard]] std::filesystem::path lockPath() const;
    [[nodiscard]] std::optional<LocalPointPageBuildResult>
    tryOpen(std::uint64_t maximumPoints,
            const LocalPointIndexBuilder::RootReady &rootReady) const;
    void prune(std::uint64_t byteBudget) const;

private:
    std::filesystem::path cacheDirectory_;
    LocalPointSourceFingerprint fingerprint_;
    std::string key_;
    std::filesystem::path finalDirectory_;
};

class LocalPageCommitter final {
public:
    static void commit(const std::filesystem::path &temporaryDirectory,
                       const std::filesystem::path &finalDirectory,
                       const LocalPointPageManifest &manifest);
};

} // namespace pci::local_index
