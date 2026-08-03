#pragma once

#include "app/PointCloudLoadMode.h"

#include <QDialog>

#include <functional>
#include <optional>

namespace pci {

class PointCloudLoadChoiceDialog final : public QDialog {
public:
    using Completion = std::function<void(std::optional<PointCloudLoadMode>)>;

    explicit PointCloudLoadChoiceDialog(QWidget *parent = nullptr);

    void openForDecision(Completion completion);

private:
    void choose(PointCloudLoadMode mode);

    Completion completion_;
    std::optional<PointCloudLoadMode> choice_;
};

} // namespace pci
