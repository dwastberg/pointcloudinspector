#include "fixtures/PdalFixtureFactory.h"

#include <filesystem>
#include <iostream>
#include <string_view>

int main(const int argc, char **argv)
{
    if (argc < 2 || argc > 3 ||
        (argc == 3 && std::string_view(argv[2]) != "--long-stress")) {
        std::cerr << "usage: pcinspector_fixture_generator <directory> "
                     "[--long-stress]\n";
        return 2;
    }

    try {
        const std::filesystem::path directory = argv[1];
        std::filesystem::remove_all(directory);
        static_cast<void>(pci::test::writePdalFixtures(directory));
        if (argc == 3) {
            static_cast<void>(
                pci::test::writePdalResidencyStressFixture(directory));
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
