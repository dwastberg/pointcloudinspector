#pragma once

#include <QGroupBox>
#include <pci/operations/StorageMaintenanceOperation.h>

class QLabel;
class QPushButton;

namespace pci {
class StorageSettingsWidget final : public QGroupBox {
public:
    explicit StorageSettingsWidget(StorageMaintenanceOperation *operation,
                                   QWidget *parent);
    void cancel();

private:
    void start(StorageMaintenanceAction action);
    void showResult(const StorageMaintenanceResult &result,
                    StorageMaintenanceAction action);
    void confirmLegacyCleanup();
    void setBusy(bool busy);
    StorageMaintenanceOperation *operation_;
    StorageMaintenanceSubscription subscription_;
    std::array<QLabel *, 3> sizes_{};
    std::array<QPushButton *, 3> folders_{};
    std::array<std::vector<std::filesystem::path>, 3> locations_;
    QLabel *summary_;
    QLabel *status_;
    QPushButton *refresh_;
    QPushButton *clean_;
    QPushButton *legacy_;
    bool closed_ = false;
    bool haveReclaimable_ = false;
    bool haveLegacy_ = false;
};
} // namespace pci
