#pragma once

#include <string>

namespace pci {

void ensureOgrRegistered();

class OgrErrorScope final {
public:
    OgrErrorScope();
    ~OgrErrorScope();
    OgrErrorScope(const OgrErrorScope &) = delete;
    OgrErrorScope &operator=(const OgrErrorScope &) = delete;

    [[nodiscard]] std::string message() const;

private:
    std::string message_;
};

} // namespace pci
