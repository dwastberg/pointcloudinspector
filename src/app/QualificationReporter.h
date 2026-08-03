#pragma once

#include "renderer/RenderMetrics.h"

#include <QString>

#include <filesystem>
#include <optional>
#include <vector>

namespace pci {

class SceneSession;

struct QualificationWriteResult {
    bool written = false;
    bool exitRequested = false;
    QString filename;
    QString error;
};

class QualificationReporter final {
public:
    void configure(std::filesystem::path outputPath,
                   bool exitAfterWrite = false);
    [[nodiscard]] bool configured() const noexcept;
    void record(const RenderMetrics &metrics);
    [[nodiscard]] QualificationWriteResult write(const QString &status,
                                                 SceneSession &session);

private:
    std::filesystem::path outputPath_;
    bool exitAfterWrite_ = false;
    bool reportWritten_ = false;
    std::optional<RenderMetrics> lastRenderMetrics_;
    std::vector<double> frameMilliseconds_;
};

} // namespace pci
