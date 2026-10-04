#include <pci/adapters/storage/ManagedStorage.h>
#include <pci/adapters/storage/SecureStorage.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <future>
#include <thread>

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
    const auto second = pci::LocalPageCacheContext::createPersistent(
        base.path() / "cache-b", configuration);

    CHECK(first->persistent());
    CHECK(second->persistent());
    CHECK(first->manifestAuthenticationKey() ==
          second->manifestAuthenticationKey());
    const std::filesystem::path key =
        configuration / "point-page-cache-auth-v1.key";
    REQUIRE(std::filesystem::is_regular_file(key));
    CHECK(std::filesystem::file_size(key) ==
          pci::manifestAuthenticationKeyBytes);
#ifndef _WIN32
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
}

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
