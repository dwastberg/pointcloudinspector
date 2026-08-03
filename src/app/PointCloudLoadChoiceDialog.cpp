#include "app/PointCloudLoadChoiceDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

namespace pci {

PointCloudLoadChoiceDialog::PointCloudLoadChoiceDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Open Point Cloud"));
    setModal(true);
    setObjectName(QStringLiteral("pointCloudLoadChoiceDialog"));

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(
        QStringLiteral("A point-cloud scene is already open."), this));

    auto *buttons = new QDialogButtonBox(this);
    auto *addButton = buttons->addButton(QStringLiteral("Add to Scene"),
                                         QDialogButtonBox::ActionRole);
    addButton->setObjectName(QStringLiteral("addToSceneButton"));
    auto *replaceButton = buttons->addButton(QStringLiteral("Open New Scene"),
                                             QDialogButtonBox::AcceptRole);
    replaceButton->setObjectName(QStringLiteral("openNewSceneButton"));
    auto *cancelButton = buttons->addButton(QDialogButtonBox::Cancel);
    cancelButton->setObjectName(QStringLiteral("cancelOpenPointCloudButton"));
    layout->addWidget(buttons);

    connect(addButton, &QPushButton::clicked, this, [this] {
        choose(PointCloudLoadMode::Add);
    });
    connect(replaceButton, &QPushButton::clicked, this, [this] {
        choose(PointCloudLoadMode::Replace);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::finished, this, [this](const int) {
        Completion completion = std::move(completion_);
        if (completion) {
            completion(choice_);
        }
    });
}

void PointCloudLoadChoiceDialog::openForDecision(Completion completion)
{
    completion_ = std::move(completion);
    choice_.reset();
    QDialog::open();
}

void PointCloudLoadChoiceDialog::choose(const PointCloudLoadMode mode)
{
    choice_ = mode;
    accept();
}

} // namespace pci
