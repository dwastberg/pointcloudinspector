#pragma once

class QApplication;

namespace pci {

// Applies the shared Strata palette and component styling. The viewport keeps
// ownership of its data background; these tokens style application chrome.
void applyStrataTheme(QApplication &application);

} // namespace pci
