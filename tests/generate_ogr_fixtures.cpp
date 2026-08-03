#include "fixtures/OgrFixtureFactory.h"

#include <iostream>

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: pcinspector_ogr_fixture_generator <directory>\n";
        return 2;
    }
    try {
        static_cast<void>(pci::test::writeOgrFixtures(argv[1]));
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
