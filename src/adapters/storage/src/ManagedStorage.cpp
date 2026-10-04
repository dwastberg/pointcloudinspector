#include <pci/adapters/platform/QtPath.h>
#include <pci/adapters/storage/ManagedStorage.h>
#include <pci/adapters/storage/SecureStorage.h>

#include <QCryptographicHash>
#include <QFile>
#include <QLockFile>
#include <QRandomGenerator>
#include <QStandardPaths>

#include <charconv>
#include <chrono>
#include <limits>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace pci {
namespace {
std::string userKey()
{
    // Windows temp and macOS temp are user-specific; the home-derived suffix
    // also isolates users on systems with a shared /tmp.
    return QCryptographicHash::hash(
               QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
                   .toUtf8(),
               QCryptographicHash::Sha256)
        .toHex()
        .left(16)
        .toStdString();
}
} // namespace

struct StorageDirectoryGuard::Impl {
    explicit Impl(const std::filesystem::path &directory)
        : lock(pathToQString(directory /
                             (".pci-storage-" + userKey() + ".lock")))
    {
        lock.setStaleLockTime(0);
    }
    QLockFile lock;
};

StorageDirectoryGuard::StorageDirectoryGuard(
    const std::filesystem::path &directory, std::stop_token stop)
    : impl_(std::make_unique<Impl>(directory))
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!impl_->lock.tryLock(0)) {
        if (stop.stop_requested() ||
            std::chrono::steady_clock::now() >= deadline ||
            impl_->lock.error() != QLockFile::LockFailedError)
            throw PrivateStorageError(
                "Storage is busy or its coordination lock is unavailable");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
StorageDirectoryGuard::~StorageDirectoryGuard() = default;

StorageLease::StorageLease(const std::filesystem::path &entry)
{
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto path = entry.parent_path() /
                    (entry.filename().string() + ".lease-" +
                     std::to_string(storageCurrentProcessId()) + "-" +
                     std::to_string(QRandomGenerator::system()->generate64()));
        QFile file(pathToQString(path));
        if (file.open(QIODeviceBase::WriteOnly | QIODeviceBase::NewOnly,
                      QFile::ReadOwner | QFile::WriteOwner)) {
            file.close();
            path_ = std::move(path);
            return;
        }
    }
    throw PrivateStorageError("Could not register storage ownership");
}
StorageLease::~StorageLease()
{
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
}

std::uint64_t storageCurrentProcessId() noexcept
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}
bool storageProcessAlive(std::uint64_t process) noexcept
{
    if (!process)
        return false;
#ifdef _WIN32
    if (process > std::numeric_limits<DWORD>::max())
        return true;
    HANDLE handle =
        OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(process));
    if (!handle)
        return GetLastError() != ERROR_INVALID_PARAMETER;
    const auto status = WaitForSingleObject(handle, 0);
    CloseHandle(handle);
    return status != WAIT_OBJECT_0;
#else
    if (process > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()))
        return true;
    errno = 0;
    return kill(static_cast<pid_t>(process), 0) == 0 || errno != ESRCH;
#endif
}

bool storageEntryProtected(const std::filesystem::path &entry)
{
    const auto prefix = entry.filename().string() + ".lease-";
    std::error_code error;
    for (std::filesystem::directory_iterator it(entry.parent_path(), error),
         end;
         !error && it != end;
         it.increment(error)) {
        const auto name = it->path().filename().string();
        if (!name.starts_with(prefix))
            continue;
        const auto suffix = std::string_view(name).substr(prefix.size());
        const auto separator = suffix.find('-');
        if (separator == std::string_view::npos)
            return true;
        if (!std::filesystem::is_regular_file(it->symlink_status(error)) ||
            error)
            return true;
        std::uint64_t nonce = 0;
        const auto random = std::from_chars(suffix.data() + separator + 1,
                                            suffix.data() + suffix.size(),
                                            nonce);
        if (random.ec != std::errc{} ||
            random.ptr != suffix.data() + suffix.size())
            return true;
        std::uint64_t pid = 0;
        const auto pidText = suffix.substr(0, separator);
        const auto result = std::from_chars(
            pidText.data(), pidText.data() + pidText.size(), pid);
        if (result.ec != std::errc{} ||
            result.ptr != suffix.data() + separator || !pid ||
            storageProcessAlive(pid))
            return true;
    }
    if (error)
        return true;
    // Builders retain their per-key lock through publication and lease
    // transfer.
    auto key = entry.filename().string();
    const auto marker = key.find(".tmp-");
    if (marker != std::string::npos)
        key.resize(marker);
    else if (entry.extension() == ".pcipages")
        key = entry.stem().string();
    else
        return false;
    const auto lockPath = entry.parent_path() / (key + ".lock");
    if (!std::filesystem::exists(lockPath, error))
        return bool(error);
    QLockFile buildLock(pathToQString(lockPath));
    buildLock.setStaleLockTime(0);
    return !buildLock.tryLock(0);
}

bool plainStorageEntry(const std::filesystem::path &path)
{
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
#else
    const auto status = std::filesystem::symlink_status(path);
    return std::filesystem::is_directory(status) ||
           std::filesystem::is_regular_file(status);
#endif
}

std::filesystem::path
managedWorkingDirectory(const std::filesystem::path &temporaryBase)
{
    auto path = std::filesystem::canonical(temporaryBase) /
                ("pcinspector-work-v1-" + userKey());
    ensurePrivateStorageDirectory(path);
    return path;
}
} // namespace pci
