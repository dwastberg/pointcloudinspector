#include "StorageSettingsWidget.h"
#include <pci/adapters/platform/QtPath.h>

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <pci/foundation/CheckedArithmetic.h>

namespace pci {
namespace {
QString sizeText(std::uint64_t bytes)
{
    return QLocale().formattedDataSize(
        static_cast<qint64>(std::min<std::uint64_t>(
            bytes,
            static_cast<std::uint64_t>(std::numeric_limits<qint64>::max()))));
}
} // namespace
StorageSettingsWidget::StorageSettingsWidget(
    StorageMaintenanceOperation *operation, QWidget *parent)
    : QGroupBox(tr("Disk Storage"), parent)
    , operation_(operation)
{
    setObjectName(QStringLiteral("diskStorageGroup"));
    auto *layout = new QVBoxLayout(this);
    auto *explanation =
        new QLabel(tr("Estimated file sizes. Active files are kept. Clearing "
                      "cached point clouds makes later imports rebuild them."),
                   this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto *rows = new QFormLayout;
    const std::array titles{tr("Point-cloud cache"),
                            tr("Temporary working files"),
                            tr("Legacy storage")};
    for (std::size_t index = 0; index < sizes_.size(); ++index) {
        auto *row = new QHBoxLayout;
        sizes_[index] = new QLabel(tr("Not scanned"), this);
        sizes_[index]->setWordWrap(true);
        sizes_[index]->setObjectName(
            QStringLiteral("storageUsage%1").arg(index));
        row->addWidget(sizes_[index], 1);
        folders_[index] = new QPushButton(tr("Open Folder"), this);
        folders_[index]->setEnabled(false);
        connect(folders_[index], &QPushButton::clicked, this, [this, index] {
            const auto open = [this](const std::filesystem::path &path) {
                if (!QDesktopServices::openUrl(
                        QUrl::fromLocalFile(pathToQString(path))))
                    status_->setText(tr("Could not open the storage folder."));
            };
            if (locations_[index].size() == 1) {
                open(locations_[index].front());
                return;
            }
            auto *menu = new QMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            for (const auto &path : locations_[index]) {
                auto *action = menu->addAction(pathToQString(path));
                connect(action, &QAction::triggered, this, [open, path] {
                    open(path);
                });
            }
            menu->popup(folders_[index]->mapToGlobal(
                QPoint(0, folders_[index]->height())));
        });
        row->addWidget(folders_[index]);
        rows->addRow(titles[index], row);
    }
    layout->addLayout(rows);
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("storageSummary"));
    summary_->setWordWrap(true);
    layout->addWidget(summary_);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("storageStatus"));
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    auto *actions = new QHBoxLayout;
    refresh_ = new QPushButton(tr("Refresh"), this);
    refresh_->setObjectName(QStringLiteral("refreshStorageButton"));
    clean_ = new QPushButton(tr("Clean unused files"), this);
    clean_->setObjectName(QStringLiteral("cleanStorageButton"));
    legacy_ = new QPushButton(tr("Clean legacy files…"), this);
    legacy_->setObjectName(QStringLiteral("cleanLegacyStorageButton"));
    actions->addWidget(refresh_);
    actions->addWidget(clean_);
    actions->addWidget(legacy_);
    layout->addLayout(actions);
    auto *immediate = new QLabel(tr("Cleanup takes effect immediately. Cancel "
                                    "and Restore Defaults do not undo it."),
                                 this);
    immediate->setWordWrap(true);
    layout->addWidget(immediate);
    connect(refresh_, &QPushButton::clicked, this, [this] {
        start(StorageMaintenanceAction::Scan);
    });
    connect(clean_, &QPushButton::clicked, this, [this] {
        start(StorageMaintenanceAction::CleanUnused);
    });
    connect(legacy_, &QPushButton::clicked, this, [this] {
        confirmLegacyCleanup();
    });
    setBusy(false);
    if (operation_)
        QTimer::singleShot(0, this, [this] {
            start(StorageMaintenanceAction::Scan);
        });
    else
        status_->setText(tr("Storage maintenance is unavailable."));
}
void StorageSettingsWidget::cancel()
{
    closed_ = true;
    subscription_.reset();
}
void StorageSettingsWidget::setBusy(bool busy)
{
    refresh_->setEnabled(operation_ && !busy);
    clean_->setEnabled(operation_ && !busy && haveReclaimable_);
    legacy_->setEnabled(operation_ && !busy && haveLegacy_);
}
void StorageSettingsWidget::start(StorageMaintenanceAction action)
{
    if (!operation_ || closed_)
        return;
    subscription_.reset();
    setBusy(true);
    status_->setToolTip({});
    status_->setText(action == StorageMaintenanceAction::Scan
                         ? tr("Scanning…")
                         : tr("Cleaning…"));
    try {
        subscription_ = operation_->start(
            action, [this, action](const StorageMaintenanceUpdate &update) {
                if (update.result) {
                    showResult(*update.result, action);
                    setBusy(false);
                } else
                    status_->setText(
                        tr("%1 %2 entries checked…")
                            .arg(action == StorageMaintenanceAction::Scan
                                     ? tr("Scanning:")
                                     : tr("Cleaning:"))
                            .arg(update.visitedEntries));
            });
    } catch (const std::exception &error) {
        status_->setText(QString::fromUtf8(error.what()));
        setBusy(false);
    }
}
void StorageSettingsWidget::showResult(const StorageMaintenanceResult &result,
                                       StorageMaintenanceAction action)
{
    std::uint64_t total = 0, available = 0, active = 0;
    for (std::size_t index = 0; index < sizes_.size(); ++index) {
        const auto &usage = result.usage[index];
        total = saturatingAdd(total, usage.totalBytes);
        available = saturatingAdd(available, usage.reclaimableBytes);
        active = saturatingAdd(active, usage.protectedBytes);
        sizes_[index]->setText(
            tr("%1 total · %2 available · %3 protected%4")
                .arg(sizeText(usage.totalBytes),
                     sizeText(usage.reclaimableBytes),
                     sizeText(usage.protectedBytes),
                     index == 2 ? tr(" · %1 unverified")
                                      .arg(sizeText(usage.unverifiedBytes))
                                : QString{}));
        locations_[index] = usage.locations;
        folders_[index]->setEnabled(!locations_[index].empty());
    }
    haveReclaimable_ = result.usage[0].reclaimableEntries != 0 ||
                       result.usage[1].reclaimableEntries != 0;
    haveLegacy_ = result.usage[2].unverifiedEntries != 0;
    summary_->setText(
        tr("%1 total · %2 available for cleanup · %3 in use or protected")
            .arg(sizeText(total), sizeText(available), sizeText(active)));
    QString status = action == StorageMaintenanceAction::Scan
                         ? tr("Scan complete.")
                         : tr("Removed approximately %1 (%2 entries).")
                               .arg(sizeText(result.removedBytes))
                               .arg(result.removedEntries);
    if (result.cancelled)
        status += tr(" Cancelled; usage is incomplete.");
    if (result.errorCount)
        status += tr(" %1 errors; usage may be incomplete. %2")
                      .arg(result.errorCount)
                      .arg(result.errors.empty()
                               ? QString{}
                               : QString::fromStdString(result.errors.front()));
    status_->setText(status);
    QStringList errors;
    for (const auto &error : result.errors)
        errors.push_back(QString::fromStdString(error));
    status_->setToolTip(errors.join(QLatin1Char('\n')));
}
void StorageSettingsWidget::confirmLegacyCleanup()
{
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("legacyStorageConfirmation"));
    dialog->setWindowTitle(tr("Clean legacy storage"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(dialog);
    auto *text =
        new QLabel(tr("Older temporary files have no verifiable owner. This "
                      "removes recognized old working files and unused "
                      "point-cloud caches. Cached point clouds will rebuild "
                      "when reopened. Source datasets and settings are kept."),
                   dialog);
    text->setWordWrap(true);
    layout->addWidget(text);
    auto *closed =
        new QCheckBox(tr("I have closed all other app instances"), dialog);
    closed->setObjectName(QStringLiteral("legacyInstancesClosedCheckBox"));
    layout->addWidget(closed);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
    auto *confirm = buttons->addButton(tr("Clean legacy files"),
                                       QDialogButtonBox::AcceptRole);
    confirm->setObjectName(QStringLiteral("confirmLegacyCleanupButton"));
    confirm->setEnabled(false);
    connect(closed, &QCheckBox::toggled, confirm, &QPushButton::setEnabled);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::accepted, this, [this] {
        start(StorageMaintenanceAction::CleanLegacy);
    });
    layout->addWidget(buttons);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->resize(480, dialog->sizeHint().height());
    dialog->open();
}
} // namespace pci
