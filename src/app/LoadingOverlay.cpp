#include "app/LoadingOverlay.h"

#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace pci {

LoadingOverlay::LoadingOverlay(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("loadingOverlay"));
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(false);
    setStyleSheet(
        QStringLiteral("QWidget#loadingOverlay {"
                       "  background-color: rgba(0, 0, 0, 120);"
                       "}"
                       "QFrame#loadingPanel {"
                       "  background-color: rgba(32, 32, 36, 235);"
                       "  border: 1px solid rgba(255, 255, 255, 70);"
                       "  border-radius: 10px;"
                       "}"
                       "QLabel { color: white; }"
                       "QProgressBar {"
                       "  min-height: 18px;"
                       "  color: white;"
                       "  text-align: center;"
                       "}"
                       "QProgressBar::chunk { background-color: #4f9cff; }"));

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(24, 24, 24, 24);
    rootLayout->addStretch(1);

    auto *centerRow = new QHBoxLayout;
    centerRow->addStretch(1);

    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("loadingPanel"));
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(24, 20, 24, 20);
    panelLayout->setSpacing(12);

    titleLabel_ = new QLabel(panel);
    titleLabel_->setObjectName(QStringLiteral("loadingTitleLabel"));
    titleLabel_->setAlignment(Qt::AlignCenter);
    titleLabel_->setWordWrap(true);
    panelLayout->addWidget(titleLabel_);

    progressBar_ = new QProgressBar(panel);
    progressBar_->setObjectName(QStringLiteral("loadingProgressBar"));
    progressBar_->setTextVisible(true);
    panelLayout->addWidget(progressBar_);

    detailsLabel_ = new QLabel(panel);
    detailsLabel_->setObjectName(QStringLiteral("loadingDetailsLabel"));
    detailsLabel_->setAlignment(Qt::AlignCenter);
    panelLayout->addWidget(detailsLabel_);

    cancelButton_ = new QPushButton(QStringLiteral("Cancel"), panel);
    cancelButton_->setObjectName(QStringLiteral("loadingCancelButton"));
    panelLayout->addWidget(cancelButton_, 0, Qt::AlignCenter);

    connect(cancelButton_, &QPushButton::clicked, this, [this] {
        setCancelling();
        emit cancelRequested();
    });

    centerRow->addWidget(panel);
    centerRow->addStretch(1);
    rootLayout->addLayout(centerRow);
    rootLayout->addStretch(1);

    hideLoading();
}

void LoadingOverlay::showLoading(const QString &sourcePath)
{
    ++presentationGeneration_;
    const QFileInfo info(sourcePath);
    sourceName_ = info.fileName().isEmpty() ? sourcePath : info.fileName();
    cancelling_ = false;
    cancelButton_->setEnabled(true);
    titleLabel_->setText(QStringLiteral("Loading %1…").arg(sourceName_));
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    detailsLabel_->setText(QStringLiteral("Reading point cloud…"));
    updatePanelWidth();
    show();
    raise();
}

void LoadingOverlay::updateProgress(const std::uint64_t processed,
                                    const std::uint64_t total)
{
    if (total == 0) {
        progressBar_->setRange(0, 0);
        detailsLabel_->setText(cancelling_
                                   ? QStringLiteral("Cancelling…")
                                   : QStringLiteral("Reading point cloud…"));
        return;
    }

    const auto percentage = static_cast<int>(
        std::min<std::uint64_t>(100, (processed * 100) / total));
    progressBar_->setRange(0, 100);
    progressBar_->setValue(percentage);
    detailsLabel_->setText(
        cancelling_ ? QStringLiteral("Cancelling… %1% | %2 / %3 points")
                          .arg(percentage)
                          .arg(processed)
                          .arg(total)
                    : QStringLiteral("%1% | %2 / %3 points")
                          .arg(percentage)
                          .arg(processed)
                          .arg(total));
}

void LoadingOverlay::setProgress(const int percentage, const QString &details)
{
    const int clamped = std::clamp(percentage, 0, 100);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(clamped);
    detailsLabel_->setText(details);
}

void LoadingOverlay::showComplete(const QString &details)
{
    cancelling_ = false;
    cancelButton_->setEnabled(false);
    setProgress(100, details);
    const std::uint64_t generation = presentationGeneration_;
    QTimer::singleShot(150, this, [this, generation] {
        if (generation == presentationGeneration_) {
            hideLoading();
        }
    });
}

void LoadingOverlay::setCancelling()
{
    cancelling_ = true;
    cancelButton_->setEnabled(false);
    detailsLabel_->setText(QStringLiteral("Cancelling…"));
}

void LoadingOverlay::hideLoading()
{
    ++presentationGeneration_;
    cancelling_ = false;
    sourceName_.clear();
    if (cancelButton_) {
        cancelButton_->setEnabled(true);
    }
    hide();
}

void LoadingOverlay::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updatePanelWidth();
}

void LoadingOverlay::updatePanelWidth()
{
    if (auto *panel = findChild<QFrame *>(QStringLiteral("loadingPanel"))) {
        const int targetWidth = std::clamp(width() - 96, 320, 560);
        panel->setFixedWidth(targetWidth);
    }
}

} // namespace pci
