#include <pci/desktop/ui/MainWindow.h>

#include <QApplication>
#include <QCloseEvent>
#include <QMessageBox>
#include <QProgressDialog>
#include <QTimer>

#include <chrono>
#include <exception>
#include <future>

namespace pci {
namespace {
class CleanupProgress final : public QProgressDialog {
public:
    CleanupProgress()
        : QProgressDialog(tr("Deleting point cloud cache files…"), {}, 0, 0)
    {
        setObjectName(QStringLiteral("cacheCleanupProgress"));
        setWindowTitle(tr("Closing Point Cloud Inspector"));
        setCancelButton(nullptr);
        setMinimumDuration(0);
    }

    void reject() override {}

protected:
    void closeEvent(QCloseEvent *event) override
    {
        event->ignore();
    }
};
} // namespace

void finishApplicationShutdown(std::unique_ptr<MainWindow> window)
{
    auto cleanup = window->takePointCacheCleanup();
    // Destruction cancels and joins import/streaming jobs, and releases all
    // source leases. The callback retains only the cache context/provider.
    window.reset();
    if (!cleanup)
        return;

    const bool quitOnClose = QApplication::quitOnLastWindowClosed();
    QApplication::setQuitOnLastWindowClosed(false);
    CleanupProgress progress;
    auto future =
        std::async(std::launch::async, [cleanup = std::move(cleanup)] {
            try {
                return cleanup();
            } catch (const std::exception &error) {
                StorageMaintenanceResult result;
                result.errorCount = 1;
                result.errors.push_back(error.what());
                return result;
            }
        });
    QTimer completion;
    completion.setInterval(20);
    QObject::connect(&completion, &QTimer::timeout, &progress, [&] {
        if (future.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready)
            progress.accept();
    });
    completion.start();
    progress.exec();
    completion.stop();
    const auto result = future.get();
    if (result.errorCount || result.skippedEntries) {
        QMessageBox message(QMessageBox::Warning,
                            QObject::tr("Cache cleanup incomplete"),
                            QObject::tr("Some cache files were kept because "
                                        "they are in use, have changed, or "
                                        "could not be deleted."),
                            QMessageBox::Ok);
        message.setObjectName(QStringLiteral("cacheCleanupWarning"));
        message.setInformativeText(
            QObject::tr("%1 cache entries deleted; %2 skipped; %3 errors.")
                .arg(result.removedEntries)
                .arg(result.skippedEntries)
                .arg(result.errorCount));
        QStringList details;
        for (const auto &error : result.errors)
            details.push_back(QString::fromStdString(error));
        message.setDetailedText(details.join(QLatin1Char('\n')));
        message.exec();
    }
    QApplication::setQuitOnLastWindowClosed(quitOnClose);
}
} // namespace pci
