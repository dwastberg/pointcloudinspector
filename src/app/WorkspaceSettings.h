#pragma once

class QMainWindow;
class QSettings;

namespace pci {

class WorkspaceSettings final {
public:
    static void restore(QMainWindow &window);
    static void save(const QMainWindow &window);

    static void restore(QMainWindow &window, const QSettings &settings);
    static void save(const QMainWindow &window, QSettings &settings);
};

} // namespace pci
