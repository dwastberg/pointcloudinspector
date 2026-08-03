#include <catch2/catch_session.hpp>

#include <QCoreApplication>

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    return Catch::Session().run(argc, argv);
}
