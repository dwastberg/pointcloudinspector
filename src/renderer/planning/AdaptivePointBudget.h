#pragma once

#include <chrono>
#include <cstdint>

namespace pci {

struct FrameSample {
    std::chrono::duration<double, std::milli> frameTime{};
    bool gpuTiming = false;
    bool includedUploads = false;
    bool includedPick = false;
    std::uint64_t submittedPoints = 0;
};

class AdaptivePointBudget {
public:
    explicit AdaptivePointBudget(std::uint64_t totalPoints);

    void update(const FrameSample &sample);
    void setTotal(std::uint64_t totalPoints);
    void setCurrent(std::uint64_t pointCount) noexcept;
    [[nodiscard]] std::uint64_t current() const noexcept;
    [[nodiscard]] std::uint64_t total() const noexcept;
    [[nodiscard]] double smoothedFrameMilliseconds() const noexcept;

private:
    std::uint64_t totalPoints_;
    std::uint64_t minimumPoints_;
    std::uint64_t currentPoints_;
    double smoothedFrameMilliseconds_ = 0.0;
    std::uint32_t consecutiveSlowSamples_ = 0;
    std::uint32_t consecutiveFastSamples_ = 0;
};

} // namespace pci
