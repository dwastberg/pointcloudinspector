#include <pci/desktop/ui/VectorSublayerDialog.h>

#include <pci/adapters/platform/QtPath.h>

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace pci {
namespace {

QString sourceTitle(const VectorImportPreflight &preflight)
{
    const QString path = pathToQString(preflight.sourcePath);
    const QString filename = QFileInfo(path).fileName();
    return filename.isEmpty() ? path : filename;
}

QString sublayerDetails(const VectorSublayerInfo &sublayer)
{
    QStringList details{
        QString::fromStdString(sublayer.geometryTypeLabel).isEmpty()
            ? QStringLiteral("Unknown geometry")
            : QString::fromStdString(sublayer.geometryTypeLabel),
    };
    if (sublayer.featureCount >= 0) {
        details << QObject::tr("%1 features")
                       .arg(QLocale().toString(
                           static_cast<qlonglong>(sublayer.featureCount)));
    } else {
        details << QObject::tr("Feature count unavailable");
    }
    details << (sublayer.hasZ ? QObject::tr("3D source")
                              : QObject::tr("2D source"));
    return details.join(QStringLiteral(" · "));
}

} // namespace

VectorSublayerDialog::VectorSublayerDialog(VectorImportPreflight preflight,
                                           QWidget *parent)
    : QDialog(parent)
    , sublayers_(std::move(preflight.sublayers))
{
    setObjectName(QStringLiteral("vectorSublayerDialog"));
    setWindowTitle(tr("Add vector layers"));
    setMinimumSize(520, 420);
    resize(620, 480);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 16);
    layout->setSpacing(12);

    auto *title = new QLabel(tr("Choose layers to add"), this);
    title->setObjectName(QStringLiteral("vectorImportHeader"));
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 2.0);
    title->setFont(titleFont);
    layout->addWidget(title);

    QString sourceText = sourceTitle(preflight);
    const QString driver = QString::fromStdString(preflight.driverName);
    if (!driver.isEmpty()) {
        sourceText += tr(" · %1").arg(driver);
    }
    auto *source = new QLabel(sourceText, this);
    source->setObjectName(QStringLiteral("vectorImportSource"));
    source->setToolTip(pathToQString(preflight.sourcePath));
    source->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(source);

    auto *instructions =
        new QLabel(tr("Each selected sublayer becomes an independently styled "
                      "overlay in the Scene."),
                   this);
    instructions->setObjectName(QStringLiteral("vectorImportInstructions"));
    instructions->setWordWrap(true);
    layout->addWidget(instructions);

    auto *selectionBar = new QHBoxLayout();
    selectionBar->setContentsMargins(0, 4, 0, 0);
    selectionSummary_ = new QLabel(this);
    selectionSummary_->setObjectName(QStringLiteral("vectorSublayerSummary"));
    selectionBar->addWidget(selectionSummary_);
    selectionBar->addStretch(1);
    auto *selectAll = new QPushButton(tr("Select all"), this);
    selectAll->setObjectName(QStringLiteral("selectAllVectorSublayersButton"));
    auto *clear = new QPushButton(tr("Clear"), this);
    clear->setObjectName(QStringLiteral("clearVectorSublayersButton"));
    selectionBar->addWidget(selectAll);
    selectionBar->addWidget(clear);
    layout->addLayout(selectionBar);

    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("vectorSublayerList"));
    list_->setAlternatingRowColors(true);
    list_->setSelectionMode(QAbstractItemView::NoSelection);
    list_->setUniformItemSizes(true);
    list_->setAccessibleName(tr("Available vector sublayers"));
    for (const auto &sublayer : sublayers_) {
        QString name = QString::fromStdString(sublayer.key.name);
        if (name.isEmpty()) {
            name = tr("Layer %1").arg(sublayer.key.index + 1);
        }
        const QString text =
            name + QStringLiteral("\n") + sublayerDetails(sublayer);
        auto *item = new QListWidgetItem(text, list_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        item->setSizeHint(QSize(0, 52));
        QString tooltip = sublayerDetails(sublayer);
        const QString crs =
            QString::fromStdString(sublayer.spatialReferenceWkt);
        if (!crs.isEmpty()) {
            tooltip += QStringLiteral("\n") + tr("Reported CRS: %1").arg(crs);
        }
        item->setToolTip(tooltip);
    }
    layout->addWidget(list_);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("vectorSublayerButtons"));
    acceptButton_ = buttons->button(QDialogButtonBox::Ok);
    acceptButton_->setProperty("primary", true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(selectAll, &QPushButton::clicked, this, [this] {
        setAllChecked(true);
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        setAllChecked(false);
    });
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem *) {
        updateSelectionSummary();
    });
    layout->addWidget(buttons);
    updateSelectionSummary();
}

std::vector<VectorSublayerKey> VectorSublayerDialog::selectedSublayers() const
{
    std::vector<VectorSublayerKey> result;
    for (int index = 0; index < list_->count(); ++index) {
        if (list_->item(index)->checkState() == Qt::Checked) {
            result.push_back(sublayers_[static_cast<std::size_t>(index)].key);
        }
    }
    return result;
}

void VectorSublayerDialog::setAllChecked(const bool checked)
{
    const QSignalBlocker blocker(list_);
    for (int index = 0; index < list_->count(); ++index) {
        list_->item(index)->setCheckState(checked ? Qt::Checked
                                                  : Qt::Unchecked);
    }
    updateSelectionSummary();
}

void VectorSublayerDialog::updateSelectionSummary()
{
    int selected = 0;
    for (int index = 0; index < list_->count(); ++index) {
        selected += list_->item(index)->checkState() == Qt::Checked ? 1 : 0;
    }
    selectionSummary_->setText(
        tr("%n of %1 selected", nullptr, selected).arg(list_->count()));
    acceptButton_->setText(selected == 1 ? tr("Add layer")
                                         : tr("Add %1 layers").arg(selected));
    acceptButton_->setEnabled(selected > 0);
}
} // namespace pci
