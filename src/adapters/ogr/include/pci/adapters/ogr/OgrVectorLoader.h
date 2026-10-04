#pragma once

#include <pci/operations/VectorImport.h>

namespace pci {

class OgrVectorLoader final : public VectorLoader {
public:
    [[nodiscard]] VectorImportPreflight
    inspect(const VectorImportRequest &request) const override;
    [[nodiscard]] std::array<double, 2>
    probeOrigin(const VectorImportRequest &request,
                std::span<const VectorSublayerKey> sublayers) const override;
    [[nodiscard]] VectorLayerDataPtr
    loadSublayer(const VectorImportRequest &request,
                 VectorSublayerKey sublayer) const override;
};

} // namespace pci
