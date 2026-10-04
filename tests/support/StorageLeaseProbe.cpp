#include <QCoreApplication>
#include <iostream>
#include <pci/adapters/platform/QtPath.h>
#include <pci/adapters/storage/ManagedStorage.h>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2)
        return 2;
    const auto path = pci::qStringToPath(app.arguments().at(1));
    std::unique_ptr<pci::StorageLease> lease;
    {
        const pci::StorageDirectoryGuard guard(path.parent_path());
        lease = std::make_unique<pci::StorageLease>(path);
    }
    std::cout << "ready\n" << std::flush;
    std::cin.get();
    return 0;
}
