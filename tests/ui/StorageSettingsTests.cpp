#include <pci/desktop/dispatch/QtCompletionExecutor.h>
#include <pci/desktop/ui/SettingsDialog.h>
#include <pci/operations/StorageMaintenanceOperation.h>

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QTest>
#include <atomic>
#include <catch2/catch_test_macros.hpp>

namespace {
class UiStorageProvider final : public pci::StorageMaintenance {
public:
    mutable std::atomic_int calls{0};
    mutable std::atomic<pci::StorageMaintenanceAction> lastAction{
        pci::StorageMaintenanceAction::Scan};
    bool block = false;
    pci::StorageMaintenanceResult run(pci::StorageMaintenanceAction action,
                                      std::stop_token stop,
                                      const Progress &) const override
    {
        ++calls;
        lastAction = action;
        while (block && !stop.stop_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        pci::StorageMaintenanceResult result;
        result.usage[0] = {.totalBytes = 4096,
                           .reclaimableBytes = 1024,
                           .protectedBytes = 3072,
                           .reclaimableEntries = 1};
        result.usage[2] = {
            .totalBytes = 256, .unverifiedBytes = 256, .unverifiedEntries = 1};
        result.removedBytes =
            action == pci::StorageMaintenanceAction::Scan ? 0 : 1024;
        return result;
    }
};
struct Fixture {
    QObject owner;
    pci::TaskScheduler scheduler{1};
    std::shared_ptr<UiStorageProvider> provider =
        std::make_shared<UiStorageProvider>();
    pci::StorageMaintenanceOperation operation{
        provider, scheduler, pci::makeQtCompletionExecutor(&owner)};
    pci::SettingsDialog dialog{
        pci::ViewportSettings{}, {}, nullptr, &operation};
    QPushButton *button(const char *name)
    {
        return dialog.findChild<QPushButton *>(QString::fromLatin1(name));
    }
    void finish()
    {
        QCoreApplication::processEvents();
        scheduler.waitForIdle();
        QCoreApplication::processEvents();
    }
};
} // namespace

TEST_CASE("settings displays disk storage and runs cleanup independently of "
          "settings preview",
          "[ui][settings][storage]")
{
    Fixture f;
    int previewChanges = 0;
    QObject::connect(&f.dialog, &pci::SettingsDialog::settingsChanged, [&] {
        ++previewChanges;
    });
    f.finish();
    REQUIRE(f.button("cleanStorageButton"));
    CHECK(f.button("cleanStorageButton")->isEnabled());
    CHECK(f.dialog.findChild<QLabel *>(QStringLiteral("storageSummary"))
              ->text()
              .contains(QStringLiteral("protected")));
    f.button("cleanStorageButton")->click();
    CHECK_FALSE(f.button("refreshStorageButton")->isEnabled());
    f.finish();
    CHECK(f.provider->lastAction == pci::StorageMaintenanceAction::CleanUnused);
    CHECK(f.dialog.findChild<QLabel *>(QStringLiteral("storageStatus"))
              ->text()
              .contains(QStringLiteral("Removed")));
    CHECK(previewChanges == 0);
    const auto calls = f.provider->calls.load();
    f.button("restoreDefaultsButton")->click();
    f.dialog.reject();
    f.finish();
    CHECK(f.provider->calls == calls);
}

TEST_CASE("legacy storage cleanup requires an explicit unchecked confirmation",
          "[ui][settings][storage]")
{
    Fixture f;
    f.finish();
    f.button("cleanLegacyStorageButton")->click();
    auto *confirmation = f.dialog.findChild<QDialog *>(
        QStringLiteral("legacyStorageConfirmation"));
    REQUIRE(confirmation);
    auto *check = confirmation->findChild<QCheckBox *>(
        QStringLiteral("legacyInstancesClosedCheckBox"));
    auto *button = confirmation->findChild<QPushButton *>(
        QStringLiteral("confirmLegacyCleanupButton"));
    REQUIRE(check);
    REQUIRE(button);
    CHECK_FALSE(check->isChecked());
    CHECK_FALSE(button->isEnabled());
    button->click();
    CHECK(f.provider->calls == 1);
    check->setChecked(true);
    button->click();
    f.finish();
    CHECK(f.provider->lastAction == pci::StorageMaintenanceAction::CleanLegacy);
}

TEST_CASE("closing settings cancels pending storage work without waiting",
          "[ui][settings][storage]")
{
    Fixture f;
    f.provider->block = true;
    QCoreApplication::processEvents();
    CHECK_FALSE(f.button("cleanStorageButton")->isEnabled());
    f.dialog.reject();
    f.scheduler.waitForIdle();
    QCoreApplication::processEvents();
    CHECK_FALSE(f.dialog.findChild<QLabel *>(QStringLiteral("storageStatus"))
                    ->text()
                    .contains(QStringLiteral("Scan complete")));
}

TEST_CASE("closing settings before its initial scan does not start a worker",
          "[ui][settings][storage]")
{
    Fixture f;
    f.dialog.reject();
    f.finish();
    CHECK(f.provider->calls == 0);
}
