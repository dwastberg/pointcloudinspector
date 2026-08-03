#pragma once

#include <QWidget>

#include <cstdint>

class QLabel;
class QProgressBar;
class QPushButton;

namespace pci {

class LoadingOverlay final : public QWidget {
    Q_OBJECT

public:
    explicit LoadingOverlay(QWidget *parent = nullptr);

    void showLoading(const QString &sourcePath);
    void updateProgress(std::uint64_t processed, std::uint64_t total);
    void setProgress(int percentage, const QString &details);
    void showComplete(const QString &details);
    void setCancelling();
    void hideLoading();

signals:
    void cancelRequested();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void updatePanelWidth();

    QLabel *titleLabel_ = nullptr;
    QLabel *detailsLabel_ = nullptr;
    QProgressBar *progressBar_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QString sourceName_;
    bool cancelling_ = false;
    std::uint64_t presentationGeneration_ = 0;
};

} // namespace pci
