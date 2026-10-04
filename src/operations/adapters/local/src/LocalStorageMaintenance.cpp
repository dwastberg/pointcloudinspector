#include <pci/adapters/storage/ManagedStorage.h>
#include <pci/foundation/CheckedArithmetic.h>
#include <pci/operations/local/LocalStorageMaintenance.h>

#include <QRandomGenerator>

#include <algorithm>
#include <array>
#include <charconv>
#include <system_error>

namespace pci {
namespace {
bool pointEntry(const std::string &name)
{
    if (name.size() < 64 ||
        !std::all_of(name.begin(), name.begin() + 64, [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        return false;
    const auto tail = std::string_view(name).substr(64);
    return tail == ".pcipages" ||
           (tail.starts_with(".tmp-") && tail.size() == 11);
}

bool workEntry(const std::string &name)
{
    for (const std::string_view prefix :
         {"pcinspector-colorize-", "pcinspector-point-pages-"}) {
        if (name.starts_with(prefix) && name.size() == prefix.size() + 6 &&
            std::all_of(
                name.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
                name.end(),
                [](unsigned char c) {
                    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9');
                }))
            return true;
    }
    return false;
}

bool stagedEntry(const std::string &name, bool points)
{
    const std::string_view prefix =
        points ? ".pci-delete-v1-p-" : ".pci-delete-v1-w-";
    if (!name.starts_with(prefix))
        return false;
    const auto suffix = std::string_view(name).substr(prefix.size());
    const auto separator = suffix.find('-');
    if (separator == std::string_view::npos)
        return false;
    std::uint64_t pid = 0, nonce = 0;
    const auto pidText = suffix.substr(0, separator);
    const auto process =
        std::from_chars(pidText.data(), pidText.data() + pidText.size(), pid);
    const auto random = std::from_chars(
        suffix.data() + separator + 1, suffix.data() + suffix.size(), nonce);
    return pid && process.ec == std::errc{} &&
           process.ptr == suffix.data() + separator &&
           random.ec == std::errc{} &&
           random.ptr == suffix.data() + suffix.size();
}

bool nestedProtected(const std::filesystem::path &entry)
{
    for (const auto &child : std::filesystem::directory_iterator(entry)) {
        if (pointEntry(child.path().filename().string()) &&
            storageEntryProtected(child.path()))
            return true;
    }
    return false;
}

void failure(StorageMaintenanceResult &result, const std::string &message)
{
    ++result.errorCount;
    if (result.errors.size() < 16)
        result.errors.push_back(message);
}

// Iterators retain only the current traversal depth. Never follow links or
// count linked targets as reclaimable storage.
std::uint64_t measure(const std::filesystem::path &path, std::stop_token stop)
{
    std::uint64_t bytes = 0;
    validatePrivateStorageDirectory(path);
    if (!plainStorageEntry(path))
        throw std::runtime_error("Linked storage entry was skipped");
    for (std::filesystem::recursive_directory_iterator it(path), end; it != end;
         ++it) {
        if (stop.stop_requested())
            break;
        if (it.depth() > 128)
            throw std::runtime_error("Storage nesting limit exceeded");
        if (!plainStorageEntry(it->path()))
            throw std::runtime_error(
                "Storage contains a link; entry was skipped");
        const auto status = it->symlink_status();
        if (std::filesystem::is_regular_file(status)) {
            if (it->hard_link_count() != 1)
                throw std::runtime_error(
                    "Storage contains a hard link; entry was skipped");
            bytes = saturatingAdd(bytes,
                                  static_cast<std::uint64_t>(it->file_size()));
        } else if (!std::filesystem::is_directory(status)) {
            throw std::runtime_error(
                "Storage contains an unexpected file type; entry was skipped");
        }
    }
    return bytes;
}

// Remove files one at a time so cancellation preserves a discoverable staging
// directory, rather than blocking shutdown in an uninterruptible remove_all.
void eraseTree(const std::filesystem::path &path,
               std::stop_token stop,
               std::uint64_t &removed,
               unsigned depth = 0)
{
    if (!plainStorageEntry(path))
        throw std::runtime_error("Storage changed to a linked entry");
    if (depth > 128)
        throw std::runtime_error("Storage nesting limit exceeded");
    for (std::filesystem::directory_iterator it(path), end; it != end; ++it) {
        if (stop.stop_requested())
            return;
        if (!plainStorageEntry(it->path()))
            throw std::runtime_error("Storage changed to a linked entry");
        if (it->is_directory()) {
            eraseTree(it->path(), stop, removed, depth + 1);
        } else if (it->is_regular_file() && it->hard_link_count() == 1) {
            const auto bytes = it->file_size();
            if (std::filesystem::remove(it->path()))
                removed =
                    saturatingAdd(removed, static_cast<std::uint64_t>(bytes));
        } else
            throw std::runtime_error(
                "Storage changed to an unexpected file type");
    }
    if (!stop.stop_requested())
        std::filesystem::remove(path);
}

class LocalStorageMaintenance final : public StorageMaintenance {
public:
    explicit LocalStorageMaintenance(StorageMaintenanceLocations locations)
        : locations_(std::move(locations))
    {
    }

    StorageMaintenanceResult run(StorageMaintenanceAction action,
                                 std::stop_token stop,
                                 const Progress &progress) const override
    {
        StorageMaintenanceResult result;
        struct Root {
            std::filesystem::path path;
            StorageCategory category;
            bool points;
            bool legacy;
        };
        const std::array roots{Root{locations_.pointCache,
                                    StorageCategory::PointCache,
                                    true,
                                    false},
                               Root{locations_.workingFiles,
                                    StorageCategory::WorkingFiles,
                                    false,
                                    false},
                               Root{locations_.legacyPointCache,
                                    StorageCategory::Legacy,
                                    true,
                                    true},
                               Root{locations_.legacyTemporaryBase,
                                    StorageCategory::Legacy,
                                    false,
                                    true}};
        std::uint64_t visited = 0;
        for (const auto &root : roots) {
            if (root.path.empty())
                continue;
            auto &usage = result.usage[static_cast<std::size_t>(root.category)];
            usage.locations.push_back(root.path);
            try {
                if (!std::filesystem::exists(root.path))
                    continue;
                if (!plainStorageEntry(root.path))
                    throw std::runtime_error("Linked storage root was skipped");
                // The shared legacy temp parent is not itself private. Every
                // recognized child must pass ownership/permissions validation.
                if (root.points || !root.legacy)
                    validatePrivateStorageDirectory(root.path);
                for (std::filesystem::directory_iterator it(root.path), end;
                     it != end;
                     ++it) {
                    if (stop.stop_requested())
                        break;
                    auto entry = it->path();
                    const auto name = entry.filename().string();
                    const bool staged = stagedEntry(name, root.points);
                    if (!staged &&
                        !(root.points ? pointEntry(name) : workEntry(name)))
                        continue;
                    try {
                        std::uint64_t bytes = measure(entry, stop);
                        if (stop.stop_requested())
                            break;
                        bool protectedEntry = false;
                        std::unique_ptr<StorageLease> deletionLease;
                        bool deleting = false;
                        {
                            const StorageDirectoryGuard gate(root.path, stop);
                            if (!std::filesystem::exists(entry))
                                continue;
                            validatePrivateStorageDirectory(entry);
                            if (!plainStorageEntry(entry))
                                throw std::runtime_error(
                                    "Storage entry changed to a link");
                            protectedEntry =
                                storageEntryProtected(entry) ||
                                (!root.points && nestedProtected(entry));
                            const bool requested =
                                root.legacy
                                    ? action ==
                                          StorageMaintenanceAction::CleanLegacy
                                    : action ==
                                          StorageMaintenanceAction::CleanUnused;
                            if (requested && !protectedEntry) {
                                // Staging stays in the same filesystem. Legacy
                                // entries also use staging, recognized below on
                                // subsequent explicitly authorized cleanup.
                                const auto destination =
                                    staged
                                        ? entry
                                        : root.path /
                                              (std::string(
                                                   root.points
                                                       ? ".pci-delete-v1-p-"
                                                       : ".pci-delete-v1-w-") +
                                               std::to_string(
                                                   storageCurrentProcessId()) +
                                               "-" +
                                               std::to_string(
                                                   QRandomGenerator::system()
                                                       ->generate64()));
                                // Only dead, well-formed leases can remain
                                // after the protection check under this same
                                // gate.
                                const auto leasePrefix =
                                    entry.filename().string() + ".lease-";
                                for (const auto &sidecar :
                                     std::filesystem::directory_iterator(
                                         root.path)) {
                                    if (sidecar.path()
                                            .filename()
                                            .string()
                                            .starts_with(leasePrefix))
                                        std::filesystem::remove(sidecar.path());
                                }
                                deletionLease =
                                    std::make_unique<StorageLease>(destination);
                                if (destination != entry)
                                    std::filesystem::rename(entry, destination);
                                entry = destination;
                                deleting = true;
                            }
                        }
                        if (deleting) {
                            try {
                                eraseTree(entry, stop, result.removedBytes);
                                if (!std::filesystem::exists(entry))
                                    ++result.removedEntries;
                            } catch (const std::exception &error) {
                                failure(result,
                                        entry.string() + ": " + error.what());
                            }
                            bytes = 0; // A fresh scan below measures all
                                       // remaining entries once.
                        }
                        usage.totalBytes =
                            saturatingAdd(usage.totalBytes, bytes);
                        auto &bucket = protectedEntry ? usage.protectedBytes
                                       : root.legacy  ? usage.unverifiedBytes
                                                      : usage.reclaimableBytes;
                        bucket = saturatingAdd(bucket, bytes);
                        if (!protectedEntry && !deleting) {
                            if (root.legacy)
                                ++usage.unverifiedEntries;
                            else
                                ++usage.reclaimableEntries;
                        }
                    } catch (const std::exception &error) {
                        if (!stop.stop_requested())
                            failure(result,
                                    entry.string() + ": " + error.what());
                    }
                    if (progress)
                        progress(++visited);
                }
            } catch (const std::exception &error) {
                if (!stop.stop_requested())
                    failure(result, root.path.string() + ": " + error.what());
            }
            if (stop.stop_requested())
                break;
        }
        if (action != StorageMaintenanceAction::Scan &&
            !stop.stop_requested()) {
            auto remaining =
                run(StorageMaintenanceAction::Scan, stop, progress);
            result.usage = std::move(remaining.usage);
            result.errorCount += remaining.errorCount;
            for (const auto &error : remaining.errors)
                if (result.errors.size() < 16)
                    result.errors.push_back(error);
        }
        result.cancelled = stop.stop_requested();
        return result;
    }

private:
    StorageMaintenanceLocations locations_;
};
} // namespace

std::shared_ptr<const StorageMaintenance>
makeLocalStorageMaintenance(StorageMaintenanceLocations locations)
{
    return std::make_shared<LocalStorageMaintenance>(std::move(locations));
}
} // namespace pci
