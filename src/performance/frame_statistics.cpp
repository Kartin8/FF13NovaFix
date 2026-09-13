#include "performance/frame_statistics.h"

#include <algorithm>
#include <cmath>

namespace novafix::performance {

void FrameStatistics::Add(float frameMs, unsigned targetRate) {
    if (!std::isfinite(frameMs) || frameMs <= 0.0f || frameMs > 10000.0f) return;

    frameTimes_[next_] = frameMs;
    const float targetMs = targetRate ? 1000.0f / static_cast<float>(targetRate) : 0.0f;
    missed_[next_] = targetMs > 0.0f && frameMs > targetMs * 1.5f;
    next_ = (next_ + 1) % FrameStatisticsSnapshot::kCapacity;
    count_ = std::min(count_ + 1, FrameStatisticsSnapshot::kCapacity);
}

FrameStatisticsSnapshot FrameStatistics::Snapshot() const {
    FrameStatisticsSnapshot result{};
    result.sampleCount = count_;
    if (!count_) return result;

    const std::size_t oldest = count_ == FrameStatisticsSnapshot::kCapacity ? next_ : 0;
    std::array<float, FrameStatisticsSnapshot::kCapacity> sorted{};
    float frameTotal = 0.0f;
    for (std::size_t i = 0; i < count_; ++i) {
        const std::size_t source = (oldest + i) % FrameStatisticsSnapshot::kCapacity;
        const float frame = frameTimes_[source];
        result.frameTimes[i] = frame;
        sorted[i] = frame;
        frameTotal += frame;
        result.worstFrameMs = std::max(result.worstFrameMs, frame);
        if (missed_[source]) ++result.missedFrames;
    }
    result.currentFrameMs = result.frameTimes[count_ - 1];
    result.averageFrameMs = frameTotal / static_cast<float>(count_);

    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count_));
    const std::size_t slowCount = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::ceil(static_cast<double>(count_) * 0.01)));
    float slowTotal = 0.0f;
    for (std::size_t i = count_ - slowCount; i < count_; ++i) slowTotal += sorted[i];
    const float slowAverage = slowTotal / static_cast<float>(slowCount);
    if (slowAverage > 0.0f) result.onePercentLowFps = 1000.0f / slowAverage;
    return result;
}

} // namespace novafix::performance
