#include <pci/rendering/planning/AdaptivePointBudget.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pci {

AdaptivePointBudget::AdaptivePointBudget(const std::uint64_t totalPoints)
    : totalPoints_(totalPoints)
    , minimumPoints_(std::min<std::uint64_t>(100'000, totalPoints))
    , currentPoints_(std::min<std::uint64_t>(1'000'000, totalPoints))
{
    if (totalPoints == 0) {
        throw std::invalid_argument("point budget requires at least one point");
    }
}

void AdaptivePointBudget::update(const FrameSample &sample)
{
    const double milliseconds = sample.frameTime.count();
    if (!std::isfinite(milliseconds) || milliseconds <= 0.0 ||
        sample.includedUploads || sample.includedPick ||
        sample.submittedPoints == 0) {
        return;
    }

    constexpr double smoothing = 0.2;
    smoothedFrameMilliseconds_ =
        smoothedFrameMilliseconds_ == 0.0
            ? milliseconds
            : (1.0 - smoothing) * smoothedFrameMilliseconds_ +
                  smoothing * milliseconds;

    if (smoothedFrameMilliseconds_ > 18.34) {
        ++consecutiveSlowSamples_;
        consecutiveFastSamples_ = 0;
    } else if (smoothedFrameMilliseconds_ < 13.34) {
        ++consecutiveFastSamples_;
        consecutiveSlowSamples_ = 0;
    } else {
        consecutiveSlowSamples_ = 0;
        consecutiveFastSamples_ = 0;
    }

    if (consecutiveSlowSamples_ >= 3) {
        const auto delta = std::max<std::uint64_t>(1, currentPoints_ / 10);
        currentPoints_ = currentPoints_ > delta ? currentPoints_ - delta : 0;
        consecutiveSlowSamples_ = 0;
    } else if (consecutiveFastSamples_ >= 6) {
        const auto delta = std::max<std::uint64_t>(1, currentPoints_ / 20);
        currentPoints_ = delta > totalPoints_ - currentPoints_
                             ? totalPoints_
                             : currentPoints_ + delta;
        consecutiveFastSamples_ = 0;
    }

    currentPoints_ = std::clamp(currentPoints_, minimumPoints_, totalPoints_);
}

void AdaptivePointBudget::setTotal(const std::uint64_t totalPoints)
{
    if (totalPoints == 0) {
        throw std::invalid_argument("point budget requires at least one point");
    }
    totalPoints_ = totalPoints;
    minimumPoints_ = std::min<std::uint64_t>(100'000, totalPoints_);
    currentPoints_ = std::clamp(currentPoints_, minimumPoints_, totalPoints_);
}

void AdaptivePointBudget::setCurrent(const std::uint64_t pointCount) noexcept
{
    currentPoints_ = std::clamp(pointCount, minimumPoints_, totalPoints_);
}

std::uint64_t AdaptivePointBudget::current() const noexcept
{
    return currentPoints_;
}

std::uint64_t AdaptivePointBudget::total() const noexcept
{
    return totalPoints_;
}

double AdaptivePointBudget::smoothedFrameMilliseconds() const noexcept
{
    return smoothedFrameMilliseconds_;
}

} // namespace pci
