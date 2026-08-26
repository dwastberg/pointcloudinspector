#include "storage/SecureStorage.h"

#include "platform/QtPath.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODeviceBase>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <aclapi.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pci {
namespace {

constexpr QFile::Permissions privateDirectoryPermissions =
    QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;

[[nodiscard]] std::filesystem::path
normalizedAbsolute(const std::filesystem::path &path)
{
    if (path.empty()) {
        throw PrivateStorageError("private storage path is empty");
    }
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) {
        throw PrivateStorageError("could not make private storage path "
                                  "absolute: " +
                                  error.message());
    }
    return absolute.lexically_normal();
}

[[nodiscard]] bool isWithin(const std::filesystem::path &path,
                            const std::filesystem::path &directory)
{
    const std::filesystem::path relative = path.lexically_relative(directory);
    if (relative.empty()) {
        return path == directory;
    }
    const auto first = relative.begin();
    return first != relative.end() && *first != "..";
}

[[nodiscard]] std::filesystem::path
validationBoundary(const std::filesystem::path &path)
{
    const QString home =
        QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (!home.isEmpty()) {
        const std::filesystem::path homePath =
            normalizedAbsolute(qStringToPath(home));
        if (isWithin(path, homePath)) {
            return homePath;
        }
    }
    return path.root_path();
}

#ifdef _WIN32

struct LocalSecurityDescriptor final {
    PSECURITY_DESCRIPTOR value = nullptr;

    ~LocalSecurityDescriptor()
    {
        if (value != nullptr) {
            LocalFree(value);
        }
    }
};

struct HandleCloser final {
    void operator()(void *handle) const noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(static_cast<HANDLE>(handle));
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

[[nodiscard]] std::vector<std::byte> currentUserSid()
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        throw PrivateStorageError("could not inspect current Windows user");
    }
    UniqueHandle token(rawToken);
    DWORD bytes = 0;
    GetTokenInformation(rawToken, TokenUser, nullptr, 0, &bytes);
    if (bytes == 0) {
        throw PrivateStorageError("could not size current Windows user SID");
    }
    std::vector<std::byte> buffer(bytes);
    if (!GetTokenInformation(
            rawToken, TokenUser, buffer.data(), bytes, &bytes)) {
        throw PrivateStorageError("could not read current Windows user SID");
    }
    const auto *user = reinterpret_cast<const TOKEN_USER *>(buffer.data());
    const DWORD sidBytes = GetLengthSid(user->User.Sid);
    std::vector<std::byte> sid(sidBytes);
    if (!CopySid(sidBytes, sid.data(), user->User.Sid)) {
        throw PrivateStorageError("could not copy current Windows user SID");
    }
    return sid;
}

[[nodiscard]] std::vector<std::byte>
wellKnownSid(const WELL_KNOWN_SID_TYPE type)
{
    DWORD bytes = SECURITY_MAX_SID_SIZE;
    std::vector<std::byte> sid(bytes);
    if (!CreateWellKnownSid(type, nullptr, sid.data(), &bytes)) {
        throw PrivateStorageError("could not create a well-known Windows SID");
    }
    sid.resize(bytes);
    return sid;
}

[[nodiscard]] bool sidEquals(PSID left, const std::vector<std::byte> &right)
{
    return left != nullptr &&
           EqualSid(left, const_cast<std::byte *>(right.data()));
}

[[nodiscard]] PSID objectAceSid(const ACCESS_ALLOWED_OBJECT_ACE &ace)
{
    const auto *cursor = reinterpret_cast<const std::byte *>(&ace.ObjectType);
    if ((ace.Flags & ACE_OBJECT_TYPE_PRESENT) != 0) {
        cursor += sizeof(GUID);
    }
    if ((ace.Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT) != 0) {
        cursor += sizeof(GUID);
    }
    return const_cast<std::byte *>(cursor);
}

void validateWindowsEntry(const std::filesystem::path &path,
                          const bool requireDirectory,
                          const bool requireCurrentOwner)
{
    const DWORD flags = requireDirectory ? FILE_FLAG_BACKUP_SEMANTICS |
                                               FILE_FLAG_OPEN_REPARSE_POINT
                                         : FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE raw =
        CreateFileW(path.c_str(),
                    FILE_READ_ATTRIBUTES | READ_CONTROL,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    flags,
                    nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        throw PrivateStorageError("could not inspect private storage path");
    }
    UniqueHandle handle(raw);
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(
            raw, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (requireDirectory &&
         (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) ||
        (!requireDirectory &&
         (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) {
        throw PrivateStorageError(
            "private storage path has an unsafe Windows file type");
    }

    PSID owner = nullptr;
    PACL dacl = nullptr;
    LocalSecurityDescriptor descriptor;
    const DWORD securityResult =
        GetSecurityInfo(raw,
                        SE_FILE_OBJECT,
                        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner,
                        nullptr,
                        &dacl,
                        nullptr,
                        &descriptor.value);
    if (securityResult != ERROR_SUCCESS || dacl == nullptr) {
        throw PrivateStorageError("private storage has no usable Windows DACL");
    }

    const std::vector<std::byte> user = currentUserSid();
    const std::vector<std::byte> system = wellKnownSid(WinLocalSystemSid);
    const std::vector<std::byte> administrators =
        wellKnownSid(WinBuiltinAdministratorsSid);
    const std::vector<std::byte> creatorOwner =
        wellKnownSid(WinCreatorOwnerSid);
    const auto authorizedOwner = [&](PSID sid) {
        return sidEquals(sid, user) || sidEquals(sid, system) ||
               sidEquals(sid, administrators);
    };
    const auto authorized = [&](PSID sid) {
        return authorizedOwner(sid) || sidEquals(sid, creatorOwner);
    };
    if ((requireCurrentOwner && !sidEquals(owner, user)) ||
        (!requireCurrentOwner && !authorizedOwner(owner))) {
        throw PrivateStorageError(
            "private storage has an unsafe Windows owner");
    }

    constexpr DWORD dangerous =
        FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_ADD_SUBDIRECTORY |
        FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE | FILE_DELETE_CHILD |
        WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void *rawAce = nullptr;
        if (!GetAce(dacl, index, &rawAce)) {
            throw PrivateStorageError("could not inspect Windows cache ACL");
        }
        const auto *header = static_cast<const ACE_HEADER *>(rawAce);
        DWORD mask = 0;
        PSID sid = nullptr;
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(rawAce);
            mask = ace->Mask;
            sid = const_cast<DWORD *>(&ace->SidStart);
        } else if (header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE) {
            const auto *ace =
                static_cast<const ACCESS_ALLOWED_OBJECT_ACE *>(rawAce);
            mask = ace->Mask;
            sid = objectAceSid(*ace);
        } else if (header->AceType == ACCESS_ALLOWED_COMPOUND_ACE_TYPE ||
                   header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
                   header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
            throw PrivateStorageError(
                "private storage has an unsupported Windows allow ACE");
        } else {
            continue;
        }
        if ((mask & dangerous) != 0 && !authorized(sid)) {
            throw PrivateStorageError(
                "private storage is writable by another Windows principal");
        }
    }
}

#else

void validatePosixEntry(const std::filesystem::path &path,
                        const bool requireDirectory,
                        const bool requireCurrentOwner,
                        const bool requireOwnerOnly)
{
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0) {
        throw PrivateStorageError("could not inspect private storage path: " +
                                  std::string(std::strerror(errno)));
    }
    if (S_ISLNK(status.st_mode) ||
        (requireDirectory && !S_ISDIR(status.st_mode)) ||
        (!requireDirectory && !S_ISREG(status.st_mode))) {
        throw PrivateStorageError("private storage path has an unsafe type");
    }
    const uid_t user = ::geteuid();
    if ((requireCurrentOwner && status.st_uid != user) ||
        (!requireCurrentOwner && status.st_uid != user && status.st_uid != 0)) {
        throw PrivateStorageError("private storage has an unexpected owner");
    }
    const bool sharedWritable = (status.st_mode & (S_IWGRP | S_IWOTH)) != 0;
    const bool protectedSharedDirectory = requireDirectory &&
                                          !requireOwnerOnly &&
                                          (status.st_mode & S_ISVTX) != 0;
    if ((sharedWritable && !protectedSharedDirectory) ||
        (requireOwnerOnly &&
         (status.st_mode & (S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)) != 0)) {
        throw PrivateStorageError("private storage permissions are too broad");
    }
}

#endif

void validateTemporaryBase(const std::filesystem::path &path)
{
#ifdef _WIN32
    validateWindowsEntry(path, true, false);
#else
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode)) {
        throw PrivateStorageError("temporary storage base has an unsafe type");
    }
    const uid_t user = ::geteuid();
    if (status.st_uid != user && status.st_uid != 0) {
        throw PrivateStorageError(
            "temporary storage base has an unexpected owner");
    }
    const bool sharedWritable = (status.st_mode & (S_IWGRP | S_IWOTH)) != 0;
    if (sharedWritable && (status.st_mode & S_ISVTX) == 0) {
        throw PrivateStorageError(
            "temporary storage base is shared-writable without sticky mode");
    }
#endif
}

void validatePrivateLeafDirectory(const std::filesystem::path &path)
{
#ifdef _WIN32
    validateWindowsEntry(path, true, true);
#else
    validatePosixEntry(path, true, true, true);
#endif
}

void validatePrivateDirectoryTree(const std::filesystem::path &path)
{
    const std::filesystem::path boundary = validationBoundary(path);
    std::filesystem::path current = path;
    bool leaf = true;
    for (;;) {
#ifdef _WIN32
        validateWindowsEntry(current, true, leaf);
#else
        validatePosixEntry(current, true, leaf, leaf);
#endif
        if (current == boundary || current == current.root_path()) {
            break;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
        current = parent;
        leaf = false;
    }
}

void ensurePrivateDirectory(const std::filesystem::path &requestedPath)
{
    const std::filesystem::path path = normalizedAbsolute(requestedPath);
    std::vector<std::filesystem::path> missing;
    std::filesystem::path current = path;
    for (;;) {
        std::error_code error;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(current, error);
        if (!error && status.type() != std::filesystem::file_type::not_found) {
            break;
        }
        if (error && error != std::errc::no_such_file_or_directory) {
            throw PrivateStorageError("could not inspect storage directory: " +
                                      error.message());
        }
        missing.push_back(current);
        const std::filesystem::path parent = current.parent_path();
        if (parent.empty() || parent == current) {
            throw PrivateStorageError(
                "private storage has no existing ancestor");
        }
        current = parent;
    }

    if (!missing.empty()) {
        // Validate the existing ancestry before creating anything underneath
        // it. The final strict owner-only check is applied after creation.
        const std::filesystem::path boundary = validationBoundary(path);
        std::filesystem::path ancestor = current;
        for (;;) {
#ifdef _WIN32
            validateWindowsEntry(ancestor, true, false);
#else
            validatePosixEntry(ancestor, true, false, false);
#endif
            if (ancestor == boundary || ancestor == ancestor.root_path()) {
                break;
            }
            ancestor = ancestor.parent_path();
        }
    }

    for (auto iterator = missing.rbegin(); iterator != missing.rend();
         ++iterator) {
        const std::filesystem::path &directory = *iterator;
        const std::filesystem::path parent = directory.parent_path();
        QDir parentDirectory(pathToQString(parent));
        if (!parentDirectory.mkdir(pathToQString(directory.filename()),
                                   privateDirectoryPermissions)) {
            // Another process may have created the same application cache
            // component after the missing-path scan. Accept only a directory;
            // the complete native ownership/type/permission validation below
            // still decides whether it is trusted.
            std::error_code error;
            if (std::filesystem::symlink_status(directory, error).type() !=
                    std::filesystem::file_type::directory ||
                error) {
                throw PrivateStorageError(
                    "could not create private directory: " +
                    directory.string());
            }
        }
    }
    validatePrivateDirectoryTree(path);
}

void validatePrivateFile(const std::filesystem::path &path)
{
#ifdef _WIN32
    validateWindowsEntry(path, false, true);
#else
    validatePosixEntry(path, false, true, true);
#endif
}

[[nodiscard]] ManifestAuthenticationKey randomAuthenticationKey()
{
    ManifestAuthenticationKey key{};
    QRandomGenerator *generator = QRandomGenerator::system();
    for (std::size_t offset = 0; offset < key.size(); offset += 4) {
        const std::uint32_t word = generator->generate();
        for (std::size_t byte = 0; byte < 4 && offset + byte < key.size();
             ++byte) {
            key[offset + byte] = static_cast<std::uint8_t>(word >> (byte * 8U));
        }
    }
    return key;
}

[[nodiscard]] ManifestAuthenticationKey
readAuthenticationKey(const std::filesystem::path &path)
{
    validatePrivateFile(path);
    QFile file(pathToQString(path));
    if (!file.open(QIODeviceBase::ReadOnly | QIODeviceBase::ExistingOnly)) {
        throw PrivateStorageError(
            "could not open point-page authentication key");
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() !=
        static_cast<qsizetype>(manifestAuthenticationKeyBytes)) {
        throw PrivateStorageError(
            "point-page authentication key has an invalid size");
    }
    ManifestAuthenticationKey key{};
    std::ranges::copy(bytes, key.begin());
    return key;
}

[[nodiscard]] ManifestAuthenticationKey loadOrCreateAuthenticationKey(
    const std::filesystem::path &configurationDirectory)
{
    const std::filesystem::path path =
        configurationDirectory / "point-page-cache-auth-v1.key";
    std::error_code error;
    if (std::filesystem::symlink_status(path, error).type() !=
        std::filesystem::file_type::not_found) {
        return readAuthenticationKey(path);
    }
    if (error && error != std::errc::no_such_file_or_directory) {
        throw PrivateStorageError(
            "could not inspect point-page authentication key");
    }

    const ManifestAuthenticationKey generated = randomAuthenticationKey();
    QTemporaryFile temporary(pathToQString(configurationDirectory /
                                           ".point-page-cache-auth-XXXXXX"));
    if (!temporary.open()) {
        throw PrivateStorageError(
            "could not create temporary point-page authentication key");
    }
    if (temporary.write(reinterpret_cast<const char *>(generated.data()),
                        static_cast<qint64>(generated.size())) !=
            static_cast<qint64>(generated.size()) ||
        !temporary.flush()) {
        throw PrivateStorageError(
            "could not write point-page authentication key");
    }
    temporary.close();
    if (temporary.rename(pathToQString(path))) {
        temporary.setAutoRemove(false);
        validatePrivateFile(path);
        return generated;
    }

    // A concurrent process may have published its key first.
    return readAuthenticationKey(path);
}

} // namespace

struct PrivateTemporaryDirectory::Impl {
    std::unique_ptr<QTemporaryDir> directory;
};

PrivateTemporaryDirectory::PrivateTemporaryDirectory(
    const std::filesystem::path &baseDirectory, std::string namePrefix)
    : impl_(std::make_unique<Impl>())
{
    const std::filesystem::path base = normalizedAbsolute(baseDirectory);
    validateTemporaryBase(base);
    impl_->directory = std::make_unique<QTemporaryDir>(
        pathToQString(base / (std::move(namePrefix) + "-XXXXXX")));
    if (!impl_->directory->isValid()) {
        throw PrivateStorageError(
            "could not create private temporary "
            "directory: " +
            impl_->directory->errorString().toStdString());
    }
    path_ = qStringToPath(impl_->directory->path());
    validatePrivateLeafDirectory(path_);
}

PrivateTemporaryDirectory::~PrivateTemporaryDirectory() = default;
PrivateTemporaryDirectory::PrivateTemporaryDirectory(
    PrivateTemporaryDirectory &&) noexcept = default;
PrivateTemporaryDirectory &PrivateTemporaryDirectory::operator=(
    PrivateTemporaryDirectory &&) noexcept = default;

const std::filesystem::path &PrivateTemporaryDirectory::path() const noexcept
{
    return path_;
}

struct LocalPageCacheContext::Impl {
    std::filesystem::path directory;
    ManifestAuthenticationKey key{};
    bool persistent = false;
    std::unique_ptr<PrivateTemporaryDirectory> temporaryDirectory;
};

LocalPageCacheContext::LocalPageCacheContext(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

LocalPageCacheContext::~LocalPageCacheContext() = default;

const std::filesystem::path &LocalPageCacheContext::directory() const noexcept
{
    return impl_->directory;
}

const ManifestAuthenticationKey &
LocalPageCacheContext::manifestAuthenticationKey() const noexcept
{
    return impl_->key;
}

bool LocalPageCacheContext::persistent() const noexcept
{
    return impl_->persistent;
}

std::shared_ptr<const LocalPageCacheContext>
LocalPageCacheContext::createPersistent(
    const std::filesystem::path &cacheDirectory,
    const std::filesystem::path &configurationDirectory)
{
    const std::filesystem::path cache = normalizedAbsolute(cacheDirectory);
    const std::filesystem::path configuration =
        normalizedAbsolute(configurationDirectory);
    ensurePrivateDirectory(cache);
    ensurePrivateDirectory(configuration);
    auto impl = std::make_unique<Impl>();
    impl->directory = cache;
    impl->key = loadOrCreateAuthenticationKey(configuration);
    impl->persistent = true;
    return std::shared_ptr<const LocalPageCacheContext>(
        new LocalPageCacheContext(std::move(impl)));
}

std::shared_ptr<const LocalPageCacheContext>
LocalPageCacheContext::createTemporary(
    const std::filesystem::path &temporaryBaseDirectory)
{
    auto impl = std::make_unique<Impl>();
    impl->temporaryDirectory = std::make_unique<PrivateTemporaryDirectory>(
        temporaryBaseDirectory, "pcinspector-point-pages");
    impl->directory = impl->temporaryDirectory->path();
    impl->key = randomAuthenticationKey();
    impl->persistent = false;
    return std::shared_ptr<const LocalPageCacheContext>(
        new LocalPageCacheContext(std::move(impl)));
}

} // namespace pci
