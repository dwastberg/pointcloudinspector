#pragma once

#include <pci/desktop/viewport/RenderMetrics.h>

#include <QString>

#include <cstddef>
#include <span>
#include <vector>

namespace pci {

struct QualificationAssertion {
    QString id;
    bool evaluated = false;
    bool passed = false;
    double observedMinimum = 0.0;
    double observedMaximum = 0.0;
    double requiredMinimum = 0.0;
    double requiredMaximum = 0.0;
    std::size_t sampleCount = 0;
    QString unit;
    QString details;
};

struct QualificationEvaluation {
    bool passed = true;
    std::vector<QualificationAssertion> assertions;
};

[[nodiscard]] QualificationEvaluation
evaluateQualification(std::span<const RenderMetrics> frames);

} // namespace pci
