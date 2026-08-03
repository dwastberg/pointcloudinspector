#include "fixtures/PdalFixtureFactory.h"

#include <filesystem>
#include <iostream>

int main(const int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: pcinspector_fixture_generator <directory>\n";
        return 2;
    }

    try {
        const std::filesystem::path directory = argv[1];
        std::filesystem::remove_all(directory);
        static_cast<void>(pci::test::writePdalFixtures(directory));
        static_cast<void>(
            pci::test::writePdalResidencyStressFixture(directory));
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
