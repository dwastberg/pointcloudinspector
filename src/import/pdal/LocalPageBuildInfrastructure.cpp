#include "import/pdal/LocalPageBuildInfrastructure.h"

#include "foundation/CheckedArithmetic.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <limits>
#include <string_view>
#include <system_error>
#include <vector>

namespace pci::local_index {
namespace {

std::filesystem::path
uniqueTemporaryDirectory(const std::filesystem::path &cacheDirectory,
                         const std::string &key)
{
    static std::atomic_uint64_t sequence = 0;
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return cacheDirectory / (key + ".tmp-" + std::to_string(nonce) + "-" +
                             std::to_string(sequence.fetch_add(1)));
}

} // namespace

LocalPageBuildLock::LocalPageBuildLock(std::filesystem::path path)
    : path_(std::move(path))
{
}

LocalPageBuildLock::~LocalPageBuildLock()
{
    if (owned_) {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
}

bool LocalPageBuildLock::tryAcquire()
{
    std::error_code error;
    owned_ = std::filesystem::create_directory(path_, error);
    if (error && error != std::make_error_code(std::errc::file_exists)) {
        throw std::runtime_error("could not create local page build lock: " +
                                 error.message());
    }
    return owned_;
}

void LocalPageBuildLock::touch() const
{
    if (!owned_) {
        return;
    }
    std::error_code error;
    std::filesystem::last_write_time(
        path_, std::filesystem::file_time_type::clock::now(), error);
    if (error) {
        throw std::runtime_error("could not refresh local page build lock");
    }
}

bool LocalPageBuildLock::removeIfStale(
    const std::chrono::minutes maximumAge) const
{
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(path_, error);
    if (error || std::filesystem::file_time_type::clock::now() - modified <=
                     maximumAge) {
        return false;
    }
    std::filesystem::remove_all(path_, error);
    return !error;
}

LocalPageBuildSession::LocalPageBuildSession(
    const std::filesystem::path &cacheDirectory, const std::string &key)
    : directory_(uniqueTemporaryDirectory(cacheDirectory, key))
{
    std::filesystem::create_directories(directory_ / "runs");
}

LocalPageBuildSession::~LocalPageBuildSession()
{
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
}

const std::filesystem::path &LocalPageBuildSession::directory() const noexcept
{
    return directory_;
}

LocalPageCacheLocator::LocalPageCacheLocator(
    const PointCloudImportPreflight &preflight,
    const LocalPointPageStoreOptions &options)
    : cacheDirectory_(options.cacheDirectory)
    , fingerprint_(fingerprintLocalPointSource(preflight.metadata,
                                               preflight.sourceFileBytes,
                                               preflight.sourceModificationTime,
                                               options))
    , key_(localPointFingerprintHex(fingerprint_))
    , finalDirectory_(cacheDirectory_ / (key_ + ".pcipages"))
{
}

const LocalPointSourceFingerprint &
LocalPageCacheLocator::fingerprint() const noexcept
{
    return fingerprint_;
}

const std::string &LocalPageCacheLocator::key() const noexcept
{
    return key_;
}

const std::filesystem::path &
LocalPageCacheLocator::finalDirectory() const noexcept
{
    return finalDirectory_;
}

std::filesystem::path LocalPageCacheLocator::lockPath() const
{
    return cacheDirectory_ / (key_ + ".lock");
}

std::optional<LocalPointPageBuildResult> LocalPageCacheLocator::tryOpen(
    const std::uint64_t maximumPoints,
    const LocalPointIndexBuilder::RootReady &rootReady) const
{
    LocalPointPageSourcePtr source;
    PointCloudNodePayloadPtr root;
    std::uint64_t payloadBytes = 0;
    try {
        source = LocalPointPageSource::openCommitted(
            finalDirectory_, fingerprint_, maximumPoints);
        root = source->loadNode(rootPointCloudNode, {});
        if (!root || pointCloudNodePayloadPoints(*root) == 0) {
            throw std::runtime_error("local page cache has an empty root");
        }
        std::error_code error;
        const std::uintmax_t measuredPayloadBytes =
            std::filesystem::file_size(finalDirectory_ / "payload.bin", error);
        payloadBytes = error ? 0 : measuredPayloadBytes;
    } catch (...) {
        return std::nullopt;
    }
    if (rootReady) {
        rootReady(source, root);
    }
    return LocalPointPageBuildResult{
        .source = std::move(source),
        .rootPayload = std::move(root),
        .storeDirectory = finalDirectory_,
        .sourcePointsScanned = 0,
        .payloadBytes = payloadBytes,
        .pageCount = 0,
        .reused = true,
    };
}

void LocalPageCacheLocator::prune(const std::uint64_t byteBudget) const
{
    if (byteBudget == 0) {
        return;
    }
    struct Entry {
        std::filesystem::path path;
        std::filesystem::file_time_type modified;
        std::uint64_t bytes = 0;
    };
    std::vector<Entry> entries;
    std::uint64_t total = 0;
    std::error_code error;
    for (const auto &directory :
         std::filesystem::directory_iterator(cacheDirectory_, error)) {
        if (error || !directory.is_directory(error) ||
            directory.path().extension() != ".pcipages") {
            continue;
        }
        std::uint64_t bytes = 0;
        for (const auto &file : std::filesystem::recursive_directory_iterator(
                 directory.path(), error)) {
            if (error) {
                break;
            }
            if (file.is_regular_file(error)) {
                const std::uintmax_t size = file.file_size(error);
                if (!error) {
                    const std::uint64_t boundedSize =
                        size > std::numeric_limits<std::uint64_t>::max()
                            ? std::numeric_limits<std::uint64_t>::max()
                            : static_cast<std::uint64_t>(size);
                    bytes = saturatingAdd(bytes, boundedSize);
                }
            }
        }
        error.clear();
        total = saturatingAdd(total, bytes);
        entries.push_back({
            .path = directory.path(),
            .modified = std::filesystem::last_write_time(
                directory.path() / "manifest.pci", error),
            .bytes = bytes,
        });
        error.clear();
    }
    std::ranges::sort(entries, [](const Entry &left, const Entry &right) {
        return left.modified < right.modified;
    });
    for (const Entry &entry : entries) {
        if (total <= byteBudget) {
            break;
        }
        if (entry.path == finalDirectory_) {
            continue;
        }
        const std::string leasePrefix =
            entry.path.filename().string() + ".lease-";
        bool leased = false;
        for (const auto &candidate :
             std::filesystem::directory_iterator(cacheDirectory_, error)) {
            if (error) {
                break;
            }
            const std::string filename = candidate.path().filename().string();
            if (!filename.starts_with(leasePrefix)) {
                continue;
            }
            const std::string_view suffix(filename.data() + leasePrefix.size(),
                                          filename.size() - leasePrefix.size());
            const std::size_t separator = suffix.find('-');
            std::uint64_t processId = 0;
            const auto parsed = std::from_chars(
                suffix.data(),
                suffix.data() + (separator == std::string_view::npos
                                     ? suffix.size()
                                     : separator),
                processId);
            if (parsed.ec == std::errc{} && localPointProcessAlive(processId)) {
                leased = true;
            } else {
                std::error_code ignored;
                std::filesystem::remove(candidate.path(), ignored);
            }
        }
        error.clear();
        if (leased) {
            continue;
        }
        std::filesystem::remove_all(entry.path, error);
        if (!error) {
            total = entry.bytes <= total ? total - entry.bytes : 0;
        }
        error.clear();
    }
}

void LocalPageCommitter::commit(const std::filesystem::path &temporaryDirectory,
                                const std::filesystem::path &finalDirectory,
                                const LocalPointPageManifest &manifest)
{
    // The manifest is the validity marker and must remain the final file
    // written before the directory is atomically published.
    writeLocalPointManifest(temporaryDirectory / "manifest.pci", manifest);

    std::error_code error;
    if (std::filesystem::exists(finalDirectory, error)) {
        std::filesystem::remove_all(finalDirectory, error);
        if (error) {
            throw std::runtime_error(
                "could not replace invalid local page cache entry");
        }
    }
    std::filesystem::rename(temporaryDirectory, finalDirectory, error);
    if (error) {
        throw std::runtime_error(
            "could not atomically commit local page cache: " + error.message());
    }
}

} // namespace pci::local_index
