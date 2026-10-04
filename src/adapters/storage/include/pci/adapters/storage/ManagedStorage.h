#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>

namespace pci {

// All cooperating publishers, readers and cleaners hold this short-lived
// interprocess gate while changing names or registering ownership.
class StorageDirectoryGuard final {
public:
    explicit StorageDirectoryGuard(const std::filesystem::path &directory,
                                   std::stop_token stop = {});
    ~StorageDirectoryGuard();
    StorageDirectoryGuard(const StorageDirectoryGuard &) = delete;
    StorageDirectoryGuard &operator=(const StorageDirectoryGuard &) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Construct while holding the parent directory gate. Retain until every
// consumer has released the entry. The sidecar survives directory publication.
class StorageLease final {
public:
    explicit StorageLease(const std::filesystem::path &entry);
    ~StorageLease();
    StorageLease(const StorageLease &) = delete;
    StorageLease &operator=(const StorageLease &) = delete;

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::uint64_t storageCurrentProcessId() noexcept;
[[nodiscard]] bool storageProcessAlive(std::uint64_t process) noexcept;
// Caller holds the parent gate. Malformed/unreadable leases protect the entry.
[[nodiscard]] bool storageEntryProtected(const std::filesystem::path &entry);
[[nodiscard]] bool plainStorageEntry(const std::filesystem::path &path);
void ensurePrivateStorageDirectory(const std::filesystem::path &directory);
void validatePrivateStorageDirectory(const std::filesystem::path &directory);
[[nodiscard]] std::filesystem::path
managedWorkingDirectory(const std::filesystem::path &temporaryBase);

} // namespace pci
