#include <pci/adapters/platform/QtPath.h>
#include <pci/adapters/storage/ManagedStorage.h>
#include <pci/adapters/storage/SecureStorage.h>
#include <pci/operations/local/LocalStorageMaintenance.h>

#include <QLockFile>
#include <QProcess>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <future>

namespace {
struct StorageFixture {
    pci::PrivateTemporaryDirectory owner{std::filesystem::temp_directory_path(),
                                         "pci-maintenance-test"};
    std::filesystem::path base = std::filesystem::canonical(owner.path());
    pci::StorageMaintenanceLocations locations{
        base / "points", base / "work", base / "legacy", base / "temp"};
    std::shared_ptr<const pci::StorageMaintenance> provider;
    StorageFixture()
    {
        for (const auto &root : {locations.pointCache,
                                 locations.workingFiles,
                                 locations.legacyPointCache,
                                 locations.legacyTemporaryBase})
            pci::ensurePrivateStorageDirectory(root);
        provider = pci::makeLocalStorageMaintenance(locations);
    }
    static std::string key(char c = 'a')
    {
        return std::string(64, c);
    }
    static std::filesystem::path add(const std::filesystem::path &root,
                                     const std::string &name,
                                     std::size_t bytes)
    {
        auto path = root / name;
        pci::ensurePrivateStorageDirectory(path);
        std::ofstream(path / "payload.bin", std::ios::binary)
            << std::string(bytes, 'x');
        return path;
    }
    pci::StorageMaintenanceResult run(pci::StorageMaintenanceAction action =
                                          pci::StorageMaintenanceAction::Scan)
    {
        return provider->run(action, {}, {});
    }
};
} // namespace

TEST_CASE("session cleanup removes only its publications and checks identity",
          "[storage][maintenance][cache-close]")
{
    StorageFixture f;
    const auto cache = pci::LocalPageCacheContext::createPersistent(
        f.locations.pointCache, f.base / "config");
    // Construct before publication to cover imports finishing during shutdown.
    const auto cleanup = pci::makeSessionPointCacheMaintenance(cache);
    const auto created =
        f.add(f.locations.pointCache, f.key() + ".pcipages", 30);
    const auto reused =
        f.add(f.locations.pointCache, f.key('b') + ".pcipages", 40);
    const auto replaced =
        f.add(f.locations.pointCache, f.key('c') + ".pcipages", 50);
    const auto missing =
        f.add(f.locations.pointCache, f.key('d') + ".pcipages", 60);
    const auto work =
        f.add(f.locations.workingFiles, "pcinspector-colorize-Ab1234", 70);
    const auto legacy =
        f.add(f.locations.legacyPointCache, f.key() + ".pcipages", 80);
    {
        const pci::StorageDirectoryGuard gate(cache->directory());
        for (const auto &entry : {created, replaced, missing})
            cache->recordCreatedEntry(entry);
    }
    std::filesystem::remove_all(missing);
    std::filesystem::remove_all(replaced);
    f.add(f.locations.pointCache, f.key('c') + ".pcipages", 90);
    const auto otherSession = pci::LocalPageCacheContext::createPersistent(
        f.locations.pointCache, f.base / "config");
    {
        const pci::StorageDirectoryGuard gate(cache->directory());
        otherSession->recordCreatedEntry(replaced);
    }
    const auto result =
        cleanup->run(pci::StorageMaintenanceAction::CleanUnused, {}, {});
    CHECK(result.errorCount == 0);
    CHECK(result.removedEntries == 1);
    CHECK(result.skippedEntries == 1);
    CHECK_FALSE(std::filesystem::exists(created));
    for (const auto &entry : {reused, replaced, work, legacy})
        CHECK(std::filesystem::exists(entry));
}

TEST_CASE("session cleanup preserves caches leased by another process",
          "[storage][maintenance][cache-close][process]")
{
    StorageFixture f;
    const auto cache = pci::LocalPageCacheContext::createPersistent(
        f.locations.pointCache, f.base / "config");
    const auto entry =
        f.add(f.locations.pointCache, f.key() + ".pcipages", 128);
    {
        const pci::StorageDirectoryGuard gate(cache->directory());
        cache->recordCreatedEntry(entry);
    }
    QProcess child;
    child.start(QString::fromUtf8(PCI_STORAGE_LEASE_PROBE),
                {pci::pathToQString(entry)});
    REQUIRE(child.waitForStarted());
    REQUIRE(child.waitForReadyRead());
    REQUIRE(child.readAllStandardOutput().contains("ready"));
    const auto cleanup = pci::makeSessionPointCacheMaintenance(cache);
    const auto protectedResult =
        cleanup->run(pci::StorageMaintenanceAction::CleanUnused, {}, {});
    CHECK(protectedResult.removedEntries == 0);
    CHECK(protectedResult.skippedEntries == 1);
    CHECK(std::filesystem::exists(entry));
    child.kill();
    REQUIRE(child.waitForFinished());
    CHECK(cleanup->run(pci::StorageMaintenanceAction::CleanUnused, {}, {})
              .removedEntries == 1);
}

TEST_CASE("session cleanup reports unsafe entries without deleting them",
          "[storage][maintenance][cache-close]")
{
    StorageFixture f;
    const auto cache = pci::LocalPageCacheContext::createPersistent(
        f.locations.pointCache, f.base / "config");
    const auto entry =
        f.add(f.locations.pointCache, f.key() + ".pcipages", 128);
    {
        const pci::StorageDirectoryGuard gate(cache->directory());
        cache->recordCreatedEntry(entry);
    }
    std::filesystem::create_hard_link(entry / "payload.bin",
                                      f.base / "linked-payload");
    const auto result = pci::makeSessionPointCacheMaintenance(cache)->run(
        pci::StorageMaintenanceAction::CleanUnused, {}, {});
    CHECK(result.errorCount != 0);
    CHECK_FALSE(result.errors.empty());
    CHECK(result.removedEntries == 0);
    CHECK(std::filesystem::exists(entry / "payload.bin"));
}

TEST_CASE("storage usage separates reclaimable active and legacy files",
          "[storage][maintenance]")
{
    StorageFixture f;
    const auto unused =
        f.add(f.locations.pointCache, f.key() + ".pcipages", 150);
    const auto active =
        f.add(f.locations.pointCache, f.key('b') + ".pcipages", 200);
    const auto work =
        f.add(f.locations.workingFiles, "pcinspector-colorize-Ab1234", 75);
    const auto legacy =
        f.add(f.locations.legacyPointCache, f.key() + ".pcipages", 300);
    const auto oldWork = f.add(
        f.locations.legacyTemporaryBase, "pcinspector-colorize-Xy1234", 50);
    const auto foreign =
        f.add(f.locations.legacyTemporaryBase, "unrelated-data", 999);
    auto lease = std::make_unique<pci::StorageLease>(active);
    auto scan = f.run();
    REQUIRE(scan.errorCount == 0);
    CHECK(scan.usage[0].totalBytes == 350);
    CHECK(scan.usage[0].reclaimableBytes == 150);
    CHECK(scan.usage[0].protectedBytes == 200);
    CHECK(scan.usage[1].reclaimableBytes == 75);
    CHECK(scan.usage[2].unverifiedBytes == 350);
    auto clean = f.run(pci::StorageMaintenanceAction::CleanUnused);
    CHECK(clean.removedBytes == 225);
    CHECK(clean.removedEntries == 2);
    CHECK(clean.usage[0].totalBytes == 200);
    CHECK_FALSE(std::filesystem::exists(unused));
    CHECK_FALSE(std::filesystem::exists(work));
    CHECK(std::filesystem::exists(active));
    CHECK(std::filesystem::exists(legacy));
    auto old = f.run(pci::StorageMaintenanceAction::CleanLegacy);
    CHECK(old.removedBytes == 350);
    CHECK_FALSE(std::filesystem::exists(oldWork));
    CHECK(std::filesystem::exists(foreign));
    lease.reset();
    CHECK(f.run(pci::StorageMaintenanceAction::CleanUnused).removedBytes ==
          200);
}

TEST_CASE("managed temporary directories protect their entire live contents",
          "[storage][maintenance]")
{
    StorageFixture f;
    const pci::PrivateTemporaryDirectory active(f.locations.workingFiles,
                                                "pcinspector-colorize");
    std::ofstream(active.path() / "run.bin") << "retained";
    auto result = f.run(pci::StorageMaintenanceAction::CleanUnused);
    CHECK(result.removedBytes == 0);
    CHECK(result.usage[1].protectedBytes == 8);
    CHECK(std::filesystem::exists(active.path() / "run.bin"));
}

TEST_CASE("cleanup keeps another process lease and reclaims it after a crash",
          "[storage][maintenance][process]")
{
    StorageFixture f;
    const auto entry =
        f.add(f.locations.pointCache, f.key() + ".pcipages", 128);
    QProcess child;
    child.start(QString::fromUtf8(PCI_STORAGE_LEASE_PROBE),
                {pci::pathToQString(entry)});
    REQUIRE(child.waitForStarted());
    REQUIRE(child.waitForReadyRead());
    REQUIRE(child.readAllStandardOutput().contains("ready"));
    CHECK(f.run(pci::StorageMaintenanceAction::CleanUnused)
              .usage[0]
              .protectedBytes == 128);
    CHECK(std::filesystem::exists(entry));
    child.kill();
    REQUIRE(child.waitForFinished());
    CHECK(f.run(pci::StorageMaintenanceAction::CleanUnused).removedBytes ==
          128);
    CHECK_FALSE(std::filesystem::exists(entry));
}

TEST_CASE("legacy fallback cleanup checks nested point cache leases",
          "[storage][maintenance]")
{
    StorageFixture f;
    const auto fallback = f.add(
        f.locations.legacyTemporaryBase, "pcinspector-point-pages-Ab1234", 0);
    const auto entry = f.add(fallback, f.key() + ".pcipages", 128);
    pci::StorageLease lease(entry);
    auto result = f.run(pci::StorageMaintenanceAction::CleanLegacy);
    CHECK(result.usage[2].protectedBytes == 128);
    CHECK(result.removedBytes == 0);
    CHECK(std::filesystem::exists(entry));
}

TEST_CASE(
    "cleanup preserves malformed ownership and ignores unrecognized entries",
    "[storage][maintenance]")
{
    StorageFixture f;
    const auto entry =
        f.add(f.locations.pointCache, f.key() + ".pcipages", 100);
    std::ofstream(entry.string() + ".lease-invalid") << "";
    const auto foreign =
        f.add(f.locations.pointCache, ".pci-delete-v1-unrelated", 200);
    const auto invalid = f.add(f.locations.pointCache, "unknown.pcipages", 300);
    auto result = f.run(pci::StorageMaintenanceAction::CleanUnused);
    CHECK(result.removedBytes == 0);
    CHECK(result.usage[0].protectedBytes == 100);
    CHECK(std::filesystem::exists(foreign));
    CHECK(std::filesystem::exists(invalid));
}

TEST_CASE("cleanup resumes staged deletion without touching linked files",
          "[storage][maintenance]")
{
    StorageFixture f;
    const auto staged =
        f.add(f.locations.workingFiles, ".pci-delete-v1-w-123-456", 64);
    const auto external = f.add(f.base, "source-data", 4096);
    const auto linked =
        f.add(f.locations.pointCache, f.key() + ".pcipages", 10);
    std::error_code error;
    std::filesystem::create_hard_link(
        external / "payload.bin", linked / "linked.bin", error);
    REQUIRE_FALSE(error);
    const auto result = f.run(pci::StorageMaintenanceAction::CleanUnused);
    CHECK(result.removedBytes == 64);
    CHECK(result.errorCount > 0);
    CHECK_FALSE(std::filesystem::exists(staged));
    CHECK(std::filesystem::exists(linked / "payload.bin"));
    CHECK(std::filesystem::file_size(external / "payload.bin") == 4096);
#ifndef _WIN32
    std::filesystem::create_directory_symlink(
        external, f.locations.pointCache / (f.key('b') + ".pcipages"));
    CHECK(f.run(pci::StorageMaintenanceAction::CleanUnused).errorCount > 0);
    CHECK(std::filesystem::exists(external / "payload.bin"));
#endif
}

TEST_CASE("storage scans missing roots and empty entries honestly",
          "[storage][maintenance]")
{
    StorageFixture f;
    std::filesystem::remove(f.locations.pointCache);
    f.add(f.locations.workingFiles, "pcinspector-colorize-Ab1234", 0);
    const auto result = f.run();
    CHECK(result.errorCount == 0);
    CHECK(result.usage[0].totalBytes == 0);
    CHECK(result.usage[1].reclaimableEntries == 1);
    CHECK(f.run(pci::StorageMaintenanceAction::CleanUnused).removedEntries ==
          1);
}

TEST_CASE(
    "storage cancellation retains completed cleanup and preserves the rest",
    "[storage][maintenance]")
{
    StorageFixture f;
    for (int index = 0; index < 200; ++index)
        f.add(f.locations.pointCache,
              ".pci-delete-v1-p-123-" + std::to_string(index),
              16);
    std::stop_source stop;
    const auto result =
        f.provider->run(pci::StorageMaintenanceAction::CleanUnused,
                        stop.get_token(),
                        [&](std::uint64_t visited) {
                            if (visited == 5)
                                stop.request_stop();
                        });
    CHECK(result.cancelled);
    CHECK(result.removedEntries == 5);
    CHECK(result.removedBytes == 80);
    const auto remaining = f.run();
    CHECK(remaining.usage[0].reclaimableEntries == 195);
    CHECK(f.run(pci::StorageMaintenanceAction::CleanUnused).removedBytes ==
          195 * 16);
}

TEST_CASE("storage coordination waits are cancellable",
          "[storage][maintenance]")
{
    StorageFixture f;
    f.add(f.locations.pointCache, f.key() + ".pcipages", 16);
    std::stop_source stop;
    stop.request_stop();
    std::future<pci::StorageMaintenanceResult> future;
    std::future_status status;
    {
        const pci::StorageDirectoryGuard gate(f.locations.pointCache);
        future = std::async(std::launch::async, [&] {
            return f.provider->run(pci::StorageMaintenanceAction::CleanUnused,
                                   stop.get_token(),
                                   {});
        });
        status = future.wait_for(std::chrono::seconds(2));
    } // Release the gate before an assertion can unwind and join the future.
    REQUIRE(status == std::future_status::ready);
    CHECK(future.get().cancelled);
    CHECK(std::filesystem::exists(f.locations.pointCache /
                                  (f.key() + ".pcipages")));
}

TEST_CASE("storage wait cleanup releases the gate before joining",
          "[storage][maintenance]")
{
    StorageFixture f;
    std::promise<void> started;
    auto ready = started.get_future();
    std::future<void> future;
    std::future_status status;
    {
        const pci::StorageDirectoryGuard gate(f.locations.pointCache);
        future = std::async(std::launch::async, [&] {
            started.set_value();
            const pci::StorageDirectoryGuard workerGate(f.locations.pointCache);
        });
        REQUIRE(ready.wait_for(std::chrono::seconds(2)) ==
                std::future_status::ready);
        status = future.wait_for(std::chrono::milliseconds(10));
    }
    CHECK(status == std::future_status::timeout);
    REQUIRE(future.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready);
    future.get();
}

TEST_CASE("cleanup rechecks leases and build locks after an earlier scan",
          "[storage][maintenance]")
{
    StorageFixture f;
    const auto entry = f.add(f.locations.pointCache, f.key() + ".pcipages", 64);
    CHECK(f.run().usage[0].reclaimableBytes == 64);
    SECTION("reader opens after scan")
    {
        pci::StorageLease lease(entry);
        CHECK(f.run(pci::StorageMaintenanceAction::CleanUnused).removedBytes ==
              0);
    }
    SECTION("builder is publishing before reader lease transfer")
    {
        QLockFile lock(
            pci::pathToQString(f.locations.pointCache / (f.key() + ".lock")));
        lock.setStaleLockTime(0);
        REQUIRE(lock.tryLock());
        CHECK(f.run(pci::StorageMaintenanceAction::CleanUnused).removedBytes ==
              0);
    }
    CHECK(std::filesystem::exists(entry));
    CHECK(f.run(pci::StorageMaintenanceAction::CleanUnused).removedBytes == 64);
}

#ifndef _WIN32
TEST_CASE(
    "partial deletion failures retain discoverable staging and can be retried",
    "[storage][maintenance]")
{
    StorageFixture f;
    const auto entry = f.add(f.locations.pointCache, f.key() + ".pcipages", 20);
    const auto blocked = f.add(entry, "blocked", 40);
    std::filesystem::permissions(blocked,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_exec);
    const auto result = f.run(pci::StorageMaintenanceAction::CleanUnused);
    // Restore permissions before assertions so the fixture always cleans up.
    for (const auto &directory :
         std::filesystem::directory_iterator(f.locations.pointCache)) {
        if (directory.is_directory()) {
            std::filesystem::permissions(directory.path() / "blocked",
                                         std::filesystem::perms::owner_all);
        }
    }
    CHECK(result.errorCount > 0);
    CHECK(result.usage[0].reclaimableBytes + result.removedBytes == 60);
    CHECK(f.run(pci::StorageMaintenanceAction::CleanUnused).removedBytes ==
          60 - result.removedBytes);
}
#endif
