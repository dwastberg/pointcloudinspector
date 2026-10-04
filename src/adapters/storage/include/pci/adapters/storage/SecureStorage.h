#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace pci {

inline constexpr std::size_t manifestAuthenticationKeyBytes = 32;
using ManifestAuthenticationKey =
    std::array<std::uint8_t, manifestAuthenticationKeyBytes>;

class PrivateStorageError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A uniquely named, owner-only directory. The directory and its contents are
// removed when the object is destroyed.
class PrivateTemporaryDirectory final {
public:
    PrivateTemporaryDirectory(const std::filesystem::path &baseDirectory,
                              std::string namePrefix);
    ~PrivateTemporaryDirectory();

    PrivateTemporaryDirectory(PrivateTemporaryDirectory &&) noexcept;
    PrivateTemporaryDirectory &operator=(PrivateTemporaryDirectory &&) noexcept;
    PrivateTemporaryDirectory(const PrivateTemporaryDirectory &) = delete;
    PrivateTemporaryDirectory &
    operator=(const PrivateTemporaryDirectory &) = delete;

    [[nodiscard]] const std::filesystem::path &path() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::filesystem::path path_;
};

struct CreatedPointCacheEntry {
    std::filesystem::path path;
    std::string identity;
};

inline constexpr const char *pointCacheCreationMarker = ".pci-creation-id";

class LocalPageCacheContext final {
public:
    ~LocalPageCacheContext();

    LocalPageCacheContext(const LocalPageCacheContext &) = delete;
    LocalPageCacheContext &operator=(const LocalPageCacheContext &) = delete;

    [[nodiscard]] const std::filesystem::path &directory() const noexcept;
    [[nodiscard]] const ManifestAuthenticationKey &
    manifestAuthenticationKey() const noexcept;
    [[nodiscard]] bool persistent() const noexcept;

    // Called only for a newly published entry, under its parent directory gate.
    // Cache hits must never be recorded. Temporary caches already own cleanup.
    void recordCreatedEntry(const std::filesystem::path &entry) const;
    [[nodiscard]] std::vector<CreatedPointCacheEntry> createdEntries() const;
    [[nodiscard]] bool hasCreatedEntries() const;

    [[nodiscard]] static std::shared_ptr<const LocalPageCacheContext>
    createPersistent(const std::filesystem::path &cacheDirectory,
                     const std::filesystem::path &configurationDirectory);
    [[nodiscard]] static std::shared_ptr<const LocalPageCacheContext>
    createTemporary(const std::filesystem::path &temporaryBaseDirectory);

private:
    struct Impl;
    explicit LocalPageCacheContext(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

using LocalPageCacheContextPtr = std::shared_ptr<const LocalPageCacheContext>;

} // namespace pci
