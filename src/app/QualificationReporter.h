#pragma once

#include "app/GdalRuntimeInfo.h"
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

struct QualificationGdalCacheSnapshot {
    std::uint64_t budgetBytes = 0;
    std::optional<std::uint64_t> usedBytes;
};

class QualificationReporter final {
public:
    void configure(std::filesystem::path outputPath,
                   bool exitAfterWrite = false);
    [[nodiscard]] bool configured() const noexcept;
    // Installed by the application layer, which owns the GDAL link.
    void setGdalRuntimeInfo(GdalRuntimeInfo info);
    void record(const RenderMetrics &metrics);
    [[nodiscard]] QualificationWriteResult
    write(const QString &status,
          SceneSession &session,
          QualificationGdalCacheSnapshot gdalCache = {});

private:
    std::filesystem::path outputPath_;
    bool exitAfterWrite_ = false;
    bool reportWritten_ = false;
    GdalRuntimeInfo gdalRuntimeInfo_;
    std::optional<RenderMetrics> lastRenderMetrics_;
    std::vector<double> frameMilliseconds_;
};

} // namespace pci
