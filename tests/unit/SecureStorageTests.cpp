#include <pci/adapters/storage/ManagedStorage.h>
#include <pci/adapters/storage/SecureStorage.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <aclapi.h>
#include <sddl.h>
#include <windows.h>
#endif

namespace {

class TemporaryBase final {
public:
    TemporaryBase()
        : owner_(std::filesystem::temp_directory_path(),
                 "pcinspector-storage-test")
        , path_(std::filesystem::canonical(owner_.path()))
    {
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

private:
    pci::PrivateTemporaryDirectory owner_;
    std::filesystem::path path_;
};

[[nodiscard]] std::string fileContents(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
}

#ifdef _WIN32
using LocalAllocation = std::unique_ptr<void, decltype(&LocalFree)>;

[[nodiscard]] LocalAllocation fileSecurity(const std::filesystem::path &path)
{
    PSECURITY_DESCRIPTOR raw = nullptr;
    const DWORD result = GetNamedSecurityInfoW(path.c_str(),
                                               SE_FILE_OBJECT,
                                               OWNER_SECURITY_INFORMATION |
                                                   DACL_SECURITY_INFORMATION,
                                               nullptr,
                                               nullptr,
                                               nullptr,
                                               nullptr,
                                               &raw);
    LocalAllocation security(raw, &LocalFree);
    REQUIRE(result == ERROR_SUCCESS);
    return security;
}

[[nodiscard]] std::string securityText(const std::filesystem::path &path)
{
    const auto security = fileSecurity(path);
    LPSTR raw = nullptr;
    const BOOL converted = ConvertSecurityDescriptorToStringSecurityDescriptorA(
        security.get(),
        SDDL_REVISION_1,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &raw,
        nullptr);
    const std::unique_ptr<char, decltype(&LocalFree)> text(raw, &LocalFree);
    REQUIRE(converted);
    return text.get();
}

[[nodiscard]] std::vector<std::byte>
tokenInformation(TOKEN_INFORMATION_CLASS informationClass)
{
    HANDLE raw = nullptr;
    REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw));
    const std::unique_ptr<void, decltype(&CloseHandle)> token(raw,
                                                              &CloseHandle);
    DWORD bytes = 0;
    GetTokenInformation(token.get(), informationClass, nullptr, 0, &bytes);
    REQUIRE(bytes > 0);
    std::vector<std::byte> buffer(bytes);
    REQUIRE(GetTokenInformation(
        token.get(), informationClass, buffer.data(), bytes, &bytes));
    return buffer;
}

void checkKeyOwner(const std::filesystem::path &path)
{
    const auto security = fileSecurity(path);
    PSID owner = nullptr;
    BOOL defaulted = FALSE;
    REQUIRE(GetSecurityDescriptorOwner(security.get(), &owner, &defaulted));
    const auto buffer = tokenInformation(TokenUser);
    const auto *user = reinterpret_cast<const TOKEN_USER *>(buffer.data());
    INFO(securityText(path));
    CHECK(EqualSid(owner, user->User.Sid));
}
#endif

} // namespace

TEST_CASE("private temporary directories are unique and remove their contents",
          "[unit][storage][security]")
{
    const TemporaryBase base;
    std::filesystem::path firstPath;
    std::filesystem::path secondPath;
    {
        pci::PrivateTemporaryDirectory first(base.path(), "operation");
        pci::PrivateTemporaryDirectory second(base.path(), "operation");
        firstPath = first.path();
        secondPath = second.path();
        REQUIRE(firstPath != secondPath);
        REQUIRE(std::filesystem::is_directory(firstPath));
        REQUIRE(std::filesystem::is_directory(secondPath));

#ifndef _WIN32
        const auto permissions =
            std::filesystem::status(firstPath).permissions();
        CHECK((permissions & std::filesystem::perms::group_all) ==
              std::filesystem::perms::none);
        CHECK((permissions & std::filesystem::perms::others_all) ==
              std::filesystem::perms::none);
#endif
    }
    CHECK_FALSE(std::filesystem::exists(firstPath));
    CHECK_FALSE(std::filesystem::exists(secondPath));
}

TEST_CASE("persistent cache contexts reuse a separately stored private key",
          "[unit][storage][security]")
{
    const TemporaryBase base;
    const std::filesystem::path configuration = base.path() / "configuration";
    const auto first = pci::LocalPageCacheContext::createPersistent(
        base.path() / "cache-a", configuration);
    const std::filesystem::path key =
        configuration / "point-page-cache-auth-v1.key";
    const auto originalContents = fileContents(key);
#ifdef _WIN32
    checkKeyOwner(key);
#endif
    const auto second = pci::LocalPageCacheContext::createPersistent(
        base.path() / "cache-b", configuration);

    CHECK(first->persistent());
    CHECK(second->persistent());
    CHECK(first->manifestAuthenticationKey() ==
          second->manifestAuthenticationKey());
    REQUIRE(std::filesystem::is_regular_file(key));
    CHECK(std::filesystem::file_size(key) ==
          pci::manifestAuthenticationKeyBytes);
    CHECK(fileContents(key) == originalContents);
#ifdef _WIN32
    checkKeyOwner(key);
#else
    const auto permissions = std::filesystem::status(key).permissions();
    CHECK((permissions & std::filesystem::perms::group_all) ==
          std::filesystem::perms::none);
    CHECK((permissions & std::filesystem::perms::others_all) ==
          std::filesystem::perms::none);
#endif
}

TEST_CASE("temporary cache contexts own and remove their cache root",
          "[unit][storage][security]")
{
    const TemporaryBase base;
    std::filesystem::path cachePath;
    {
        const auto cache =
            pci::LocalPageCacheContext::createTemporary(base.path());
        cachePath = cache->directory();
        CHECK_FALSE(cache->persistent());
        CHECK(std::filesystem::is_directory(cachePath));
    }
    CHECK_FALSE(std::filesystem::exists(cachePath));
}

TEST_CASE("cache creation records are session scoped and survive reopening",
          "[unit][storage][cache-close]")
{
    const TemporaryBase base;
    const auto cache = pci::LocalPageCacheContext::createPersistent(
        base.path() / "cache", base.path() / "config");
    const auto entry =
        cache->directory() / (std::string(64, 'a') + ".pcipages");
    pci::ensurePrivateStorageDirectory(entry);
    {
        const pci::StorageDirectoryGuard gate(cache->directory());
        cache->recordCreatedEntry(entry);
    }
    REQUIRE(cache->createdEntries().size() == 1);
    CHECK(cache->createdEntries().front().path == entry);
    CHECK_FALSE(cache->createdEntries().front().identity.empty());
    CHECK(cache->hasCreatedEntries());
    const auto reopened = pci::LocalPageCacheContext::createPersistent(
        cache->directory(), base.path() / "config");
    CHECK(reopened->createdEntries().empty());
    CHECK_FALSE(reopened->hasCreatedEntries());
    std::filesystem::remove_all(entry);
    CHECK_FALSE(cache->hasCreatedEntries());
    CHECK(cache->createdEntries().size() == 1);

    const auto temporary =
        pci::LocalPageCacheContext::createTemporary(base.path());
    const auto temporaryEntry = temporary->directory() / "entry";
    pci::ensurePrivateStorageDirectory(temporaryEntry);
    temporary->recordCreatedEntry(temporaryEntry);
    CHECK(temporary->createdEntries().empty());
}

TEST_CASE("concurrent cache initialization publishes one authentication key",
          "[unit][storage][security][race]")
{
    const TemporaryBase base;
    const std::filesystem::path configuration = base.path() / "configuration";
    std::atomic_uint32_t ready = 0;
    std::atomic_bool start = false;
    const auto create = [&](const std::string &cacheName) {
        ready.fetch_add(1);
        while (!start.load()) {
            std::this_thread::yield();
        }
        return pci::LocalPageCacheContext::createPersistent(
            base.path() / cacheName, configuration);
    };
    auto first = std::async(std::launch::async, create, "cache-a");
    auto second = std::async(std::launch::async, create, "cache-b");
    while (ready.load() != 2) {
        std::this_thread::yield();
    }
    start.store(true);

    const auto firstContext = first.get();
    const auto secondContext = second.get();
    CHECK(firstContext->manifestAuthenticationKey() ==
          secondContext->manifestAuthenticationKey());
    const auto reopened = pci::LocalPageCacheContext::createPersistent(
        base.path() / "cache-c", configuration);
    CHECK(reopened->manifestAuthenticationKey() ==
          firstContext->manifestAuthenticationKey());
#ifdef _WIN32
    checkKeyOwner(configuration / "point-page-cache-auth-v1.key");
#endif
}

TEST_CASE("persistent cache rejects an unsafe existing key without changing it",
          "[unit][storage][security]")
{
    const TemporaryBase base;
    const auto configuration = base.path() / "configuration";
    const auto cache = base.path() / "cache";
    const auto initial =
        pci::LocalPageCacheContext::createPersistent(cache, configuration);
    const auto key = configuration / "point-page-cache-auth-v1.key";
    const auto originalContents = fileContents(key);
#ifdef _WIN32
    PSECURITY_DESCRIPTOR raw = nullptr;
    const BOOL converted = ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;FA;;;WD)", SDDL_REVISION_1, &raw, nullptr);
    const LocalAllocation unsafeSecurity(raw, &LocalFree);
    REQUIRE(converted);
    PACL dacl = nullptr;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    REQUIRE(GetSecurityDescriptorDacl(
        unsafeSecurity.get(), &present, &dacl, &defaulted));
    REQUIRE(present);
    REQUIRE(SetNamedSecurityInfoW(const_cast<wchar_t *>(key.c_str()),
                                  SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION |
                                      PROTECTED_DACL_SECURITY_INFORMATION,
                                  nullptr,
                                  nullptr,
                                  dacl,
                                  nullptr) == ERROR_SUCCESS);
    const auto originalSecurity = securityText(key);
#else
    std::filesystem::permissions(key,
                                 std::filesystem::perms::group_write,
                                 std::filesystem::perm_options::add);
    const auto originalPermissions = std::filesystem::status(key).permissions();
#endif
    CHECK_THROWS_AS(
        pci::LocalPageCacheContext::createPersistent(cache, configuration),
        pci::PrivateStorageError);
    CHECK(fileContents(key) == originalContents);
#ifdef _WIN32
    CHECK(securityText(key) == originalSecurity);
#else
    CHECK(std::filesystem::status(key).permissions() == originalPermissions);
#endif
}

#ifdef _WIN32
TEST_CASE("persistent cache does not adopt a key owned by the token's group",
          "[unit][storage][security]")
{
    const auto ownerBuffer = tokenInformation(TokenOwner);
    const auto userBuffer = tokenInformation(TokenUser);
    const auto *owner =
        reinterpret_cast<const TOKEN_OWNER *>(ownerBuffer.data());
    const auto *user = reinterpret_cast<const TOKEN_USER *>(userBuffer.data());
    if (EqualSid(owner->Owner, user->User.Sid)) {
        SKIP("Requires a Windows token with a group as its default owner");
    }
    const TemporaryBase base;
    const auto configuration = base.path() / "configuration";
    const auto cache = base.path() / "cache";
    const auto initial =
        pci::LocalPageCacheContext::createPersistent(cache, configuration);
    const auto key = configuration / "point-page-cache-auth-v1.key";
    REQUIRE(SetNamedSecurityInfoW(const_cast<wchar_t *>(key.c_str()),
                                  SE_FILE_OBJECT,
                                  OWNER_SECURITY_INFORMATION,
                                  owner->Owner,
                                  nullptr,
                                  nullptr,
                                  nullptr) == ERROR_SUCCESS);
    const auto originalContents = fileContents(key);
    const auto originalSecurity = securityText(key);
    CHECK_THROWS_AS(
        pci::LocalPageCacheContext::createPersistent(cache, configuration),
        pci::PrivateStorageError);
    CHECK(fileContents(key) == originalContents);
    CHECK(securityText(key) == originalSecurity);
}
#endif

#ifndef _WIN32
TEST_CASE("persistent cache contexts reject linked and broadly writable roots",
          "[unit][storage][security]")
{
    const TemporaryBase base;
    const std::filesystem::path configuration = base.path() / "configuration";
    const std::filesystem::path target = base.path() / "target";
    REQUIRE(std::filesystem::create_directory(target));
    const std::filesystem::path linked = base.path() / "linked";
    std::filesystem::create_directory_symlink(target, linked);
    CHECK_THROWS_AS(
        pci::LocalPageCacheContext::createPersistent(linked, configuration),
        pci::PrivateStorageError);

    const std::filesystem::path writable = base.path() / "writable";
    REQUIRE(std::filesystem::create_directory(writable));
    std::filesystem::permissions(writable,
                                 std::filesystem::perms::group_write,
                                 std::filesystem::perm_options::add);
    CHECK_THROWS_AS(
        pci::LocalPageCacheContext::createPersistent(writable, configuration),
        pci::PrivateStorageError);
    CHECK_THROWS_AS(pci::PrivateTemporaryDirectory(writable, "operation"),
                    pci::PrivateStorageError);
    CHECK_THROWS_AS(pci::PrivateTemporaryDirectory(linked, "operation"),
                    pci::PrivateStorageError);

    const std::filesystem::path sticky = base.path() / "sticky";
    REQUIRE(std::filesystem::create_directory(sticky));
    std::filesystem::permissions(sticky,
                                 std::filesystem::perms::owner_all |
                                     std::filesystem::perms::group_all |
                                     std::filesystem::perms::others_all |
                                     std::filesystem::perms::sticky_bit,
                                 std::filesystem::perm_options::replace);
    CHECK_NOTHROW(pci::LocalPageCacheContext::createPersistent(
        sticky / "private-cache", configuration));
}
#endif
