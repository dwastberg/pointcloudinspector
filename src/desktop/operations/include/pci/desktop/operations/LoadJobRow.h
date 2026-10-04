#pragma once

#include <pci/operations/LoadJobKey.h>

#include <QMetaType>
#include <QString>

namespace pci {

struct LoadJobRow {
    LoadJobKey key;
    QString title;
    QString detail;
    double completion = 0.0;
    bool terminal = false;
    LoadJobCapabilities capabilities;
};

} // namespace pci

Q_DECLARE_METATYPE(pci::LoadJobRow)
Q_DECLARE_METATYPE(pci::LoadJobKey)
Q_DECLARE_METATYPE(pci::LoadJobId)
