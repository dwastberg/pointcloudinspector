#pragma once
namespace pci {
class SceneSession;
// Installs the desktop transaction acknowledgments and presentation callbacks.
struct SceneOperationBindings {
    static void connectController(SceneSession &session);
    static void connectVectorController(SceneSession &session);
    static void connectRasterController(SceneSession &session);
    static void connectRasterElevationController(SceneSession &session);
    static void connectColorizeController(SceneSession &session);
};
} // namespace pci
