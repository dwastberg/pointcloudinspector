#include <catch2/catch_session.hpp>

#include <QApplication>

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Exercise widget behavior with synchronous Qt painting. The GUI pool in
    // prebuilt Qt libraries is outside our sanitizer instrumentation;
    // application workers remain concurrent. Native GPU tests use a separate
    // entry point.
    qputenv("QT_NO_GUI_THREADPOOL", "1");
    QApplication application(argc, argv);
    return Catch::Session().run(argc, argv);
}
