#pragma once

#include <array>
#include <cstddef>

namespace novafix::performance {

struct FrameStatisticsSnapshot {
    static constexpr std::size_t kCapacity = 240;

    std::array<float, kCapacity> frameTimes{};
    std::size_t sampleCount{};
    float currentFrameMs{};
    float averageFrameMs{};
    float onePercentLowFps{};
    float worstFrameMs{};
    unsigned missedFrames{};
};

class FrameStatistics {
public:
    void Add(float frameMs, unsigned targetRate);
    FrameStatisticsSnapshot Snapshot() const;

private:
    std::array<float, FrameStatisticsSnapshot::kCapacity> frameTimes_{};
    std::array<bool, FrameStatisticsSnapshot::kCapacity> missed_{};
    std::size_t next_{};
    std::size_t count_{};
};

} // namespace novafix::performance
